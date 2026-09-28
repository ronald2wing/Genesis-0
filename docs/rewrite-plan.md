# Genesis-0 — Concat Rewrite Plan (Planning Document)

Status: **planning / not implemented**. All artifacts below are **PLANNED**; no file other than this
document is created by it. Checked: 2026-09-28. No release, performance, or compliance claim is made.
Effort figures are order-of-magnitude planning inputs, **not** schedules or promises.

---

## 0. How to read this plan

- Section 1 fixes what "rewrite Concat" means and what it does not.
- Section 2 maps each measured Concat crate to a Genesis-0 module and a verdict.
- Section 3 is the load-bearing section: where Concat's architecture does **not** map onto GES/Qt.
- Section 4 orders the work; **Phase 0 gates every later phase**.
- Sections 5–7 are sizing, the immediate next step, and decisions the user must make.

Concat size, measured from the checkout (`concat/src/crates/*`): **84,370 Rust lines** across 132
files, **27,743 Slint lines** across 63 files, **1,744 WGSL lines** across 98 files, and **123 effect
packages** under `concat/src/crates/concat-effects/packages/`. A workspace scan finds **16 crates**,
not 17 (see §2 note and §7 Q1).

## 1. Scope statement

**In scope.** Re-derive, in C++23 and Qt6 Quick/QML, the _observable behavior_ of Concat: its project
document and editing commands, its timeline flatten/resolve stages, its preview/export loop, its
effect catalogue and packages, its masks/vision and speech features, its API surface, and its desktop
and phone workflows — expressed through Genesis-0's host model and engine adapter, with GES as the
primary engine and native adapters on mobile.

**Explicitly not in scope.**

- **No second engine and no custom compositor by default.** `docs/architecture.md` §6 fixes GES as
  primary; `wb/wgpu`/`Skia` are listed as "only if a custom compositor is ever needed; not selected".
  Whether Concat's compositor forces that selection is exactly the Phase 0 gate, not a decision taken
  here.
- **No mobile implementation on the desktop engine.** Mobile is served by native adapters
  (AVFoundation/Media3) per `docs/decisions/engine-choice.md` §4; Concat's Android host does not
  transfer.
- **No performance, parity, or store claim.** The GES desktop render spike passed (2026-09-29,
  `docs/decisions/compositor-path.md`); mobile is untested (`docs/decisions/engine-choice.md` §2).
- **No new dependencies** (models, runtimes, libraries) are selected here; AI models and the
  sandbox runtime remain "not selected" (`docs/dependencies.md`).

"Rewrite Concat" therefore means: **port the behavior and data concepts, replace the engine,
compositor, and UI, and keep the host portable.**

## 2. Crate → module mapping

Verdicts: **port** = re-derive logic in C++23 largely as-is; **re-derive** = same behavior, different
substrate/algorithm; **replace** = the substrate changes to Qt/GES and little code carries over;
**drop** = not carried; **defer** = planned later, not in the initial slice.

| Concat crate      | Rust LOC | Role (from `concat/ARCHITECTURE.md` §1)                                                                                           | Genesis-0 module (planned)                                    | Verdict       | Justification                                                                                                                                                                                                           |
| ----------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- | ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `concat-core`     | 2,829    | time (`Rational`), arena, frame, timeline, shader types, retime, animate; std-only, wasm                                          | `core/time/`, `core/model/`, `core/effects/` types            | **port**      | Pure algorithms (rational time, arena, retime, animation). No IO. Maps directly onto R5's host-owned rational time (`docs/architecture.md` §7). `concat/<crate>/src/time.rs`, `timeline.rs`, `retime.rs`, `animate.rs`. |
| `concat-project`  | 9,192    | document model, commands, undo, serde, migrations (`model.rs`, `commands/`, `editor.rs`, `doc.rs`)                                | `core/project/`, `core/undo/`, `core/effects/`                | **re-derive** | Document concepts port; **undo must change** from snapshot to command stack (architecture §7) — see §3(c); times must change from `f64` seconds to rational — see §3(d).                                                |
| `concat-media`    | 9,386    | FFmpeg probe/decode/encode/pool/prefetch/hardware (`decode.rs`, `pool.rs`, `prefetch.rs`, `hardware.rs`)                          | `adapters/engine/` (+ GES)                                    | **replace**   | The host does **not** re-implement codecs (R5); GES/GStreamer owns decode/encode. Cache/prefetch/proxy _semantics_ are re-derived on the adapter; the `ffmpeg-the-third` binding is not.                                |
| `concat-effects`  | 7,090    | effect packages (`effect.toml` format 2 + `effect.wgsl`), catalogue, naga validation, budgets                                     | `core/effects/` + Extension/Pack pipeline                     | **re-derive** | Manifest/param concepts map to the Extension/Pack control schema; the WGSL multi-pass execution model has no GES equivalent — see §3(b). `concat/<crate>/src/manifest.rs`, `catalogue.rs`, `shader.rs`.                 |
| `concat-render`   | 8,044    | `FramePlan`, GPU compositor, scopes, SSIM (`plan.rs`, `gpu.rs`, `scopes.rs`, `reference.rs`)                                      | GES compositor + `adapters/engine/` FramePlan bridge          | **replace**   | The custom wgpu compositor is displaced by GES's compositor; whether the `FramePlan` contract can be preserved as an adapter boundary is a Phase 0 question — see §3(a).                                                |
| `concat-vision`   | 2,760    | cutout masks, brushes, enhance (ONNX) (`mask.rs`, `brush.rs`, `enhance.rs`)                                                       | `core/masks/`, `core/tracking/`, AI job runtime               | **defer**     | ONNX Runtime is not selected; masks/rotoscoping are Phase 3 of `docs/implementation-plan.md`. Behavior re-derived later; the `ort` dependency is not carried.                                                           |
| `concat-text`     | 1,635    | title rasteriser                                                                                                                  | Qt text rendering (`QTextDocument`/`QPainter`) or Pack        | **replace**   | Qt owns text/font layout/rasterisation; the Slint-era rasteriser is not portable.                                                                                                                                       |
| `concat-export`   | 3,498    | document → engine timeline, render loop, preview (`flatten.rs`, `resolve.rs`, `lib.rs`)                                           | `adapters/engine/`                                            | **re-derive** | This is the core of the port: flatten (`Vec<ExportClip>`) and resolve (rational `Timeline`) become the adapter that compiles the host graph into a GES pipeline. Behavior preserved; GES types never leak (R6).         |
| `concat-host`     | 11,540   | Session, projects, playback, monitor, proxies, scheduler, jobs (`session.rs`, `playback.rs`, `preview.rs`, `proxy.rs`, `jobs.rs`) | `core/` services + `adapters/` + `core/jobs/`                 | **re-derive** | Session/proxy/job _semantics_ port to the host; playback/monitor/scheduler are re-implemented on GES + Qt worker threads (R4: no media work on the UI thread).                                                          |
| `concat-speech`   | 3,094    | whisper.cpp transcription, Kokoro TTS, chatterbox (`transcribe.rs`, `tts.rs`)                                                     | `ai/providers/` (AI job contract)                             | **defer**     | C++ providers/models are not selected (`docs/architecture.md` §12). The job contract (submit/status/progress/cancel/result) ports; the Rust model crates do not.                                                        |
| `concat-api`      | 2,350    | one dispatcher, every verb as JSON (`message.rs`, `lib.rs`)                                                                       | host command/API layer                                        | **re-derive** | Concat's "every verb as JSON" aligns naturally with Genesis-0's command model; re-derive as the host command surface. Optional remote exposure is §7 Q7.                                                                |
| `concat-server`   | 1,376    | JSON-RPC lines, gRPC, tokens (`lib.rs`, `json.rs`, `grpc.rs`, `token.rs`)                                                         | — (out of initial scope)                                      | **drop**      | Not required by any Genesis-0 decision register entry; a remote server with gRPC and token minting is product scope not yet chosen.                                                                                     |
| `concat-cli`      | 514      | command-line driver                                                                                                               | — (defer to tooling)                                          | **defer**     | A thin client over the API; revisit after the API layer exists.                                                                                                                                                         |
| `concat` (window) | 19,635   | Slint window, panes, studio controller, monitor (`studio.rs`, `panes/`, `gpu.rs`)                                                 | `ui/qml/`                                                     | **replace**   | 27.7k Slint lines are not portable; the pane state/message model is re-derived in QML/C++. Monitor texture integration is the Phase 0 gate — see §3(e).                                                                 |
| `concat-android`  | 335      | Android activity hosting the Slint window                                                                                         | native mobile adapters (`docs/decisions/engine-choice.md` §4) | **drop**      | Slint's `android-activity` backend is specific to Concat; Genesis-0 mobile is Qt + Media3/AVFoundation.                                                                                                                 |
| `concat-perf`     | 1,092    | performance table (`main.rs`)                                                                                                     | `benchmarks/`                                                 | **re-derive** | Budgets and scenarios are re-derived as benchmarks against the fixtures of `docs/implementation-plan.md` Phase 0; no Concat number is inherited.                                                                        |

**Note on the crate count.** The brief states 17 crates and 16 per-crate LOC figures; the checkout's
workspace (`concat/src/Cargo.toml`, `members = ["crates/*"]`) contains **16** members and the mapping
table above covers all 16. If a 17th crate is intended, it must be named (§7 Q1); no such member is
present in the read-only checkout.

## 3. Feasibility risks

This section is the point of the document. Each item is where Concat's architecture **does not** map
onto GES/Qt, with the least-optimistic reading.

### (a) Custom `FramePlan` + wgpu compositor vs GES's compositor — HIGHEST RISK

Concat's renderer is a first-class custom pipeline: `plan_frame(t)` produces a pure `FramePlan`
(`concat/<crate>/src/plan.rs:434`) describing visible layers, source timestamps, placement, opacity,
blend and per-layer treatments; the executor fills it with decoded pictures and resolved effects; a
single `WgpuCompositor` draws every frame (monitor and export) with a working space of **linear light,
Rec.709 primaries, extended scRGB, half floats**, converting on upload (`COPY_SHADER`, `DEEP_SHADER`)
and rolling off HDR with BT.2390.

GES composites through GStreamer's `compositor`/`videoaggregator` and `gnlcomposition`, not through a
programmable `FramePlan`. The mismatches are specific:

- **Multi-pass WGSL effects.** A format-2 package declares named passes (`[[wgsl.pass]] target=...,
shrink=[...]`, e.g. `concat.gaussian-blur/effect.toml`), intermediate pictures, and `<target>_at`
  reads. GES/GStreamer has **no** equivalent named-intermediate multi-pass shader graph **in its
  stock elements** — see the settled update below; a custom element does express it. The nearest
  primitives are the `glshader` element (single-pass **GLSL**, not WGSL) and per-element GLSL filters.
  Reproducing 98 shader packages on GES elements is not a port; it is a re-authoring.
  **Spike update (2026-09-28):** chained `glshader` elements _do_ compose — two passes ran and
  produced GPU-resident `GLMemory` frames. So a _linear_ pass chain is expressible as a GStreamer
  element chain.
  **Settled (2026-09-29, `spikes/gl-multipass/`):** branching is expressible — `tee` → two
  `glshader` → `glvideomixer` recombines GPU-resident (`GLMemory`, `texture-target=2D`), pixel-exact
  — but the **named-intermediate** model is **not**: a later pass cannot read an earlier pass's
  output, `glshader` binds exactly one texture per element, and a second `sampler2D` **silently
  aliases texture unit 0** with no error. `Stage::shrink` is likewise unrepresentable (`glshader` is
  a base transform, identical in/out dimensions). Folding stages into one shader body approximates
  the look only when every stage reads the same source. See `docs/decisions/effects-execution.md` §6.
- **Working space.** GES does not expose an extended-linear half-float compositing space; GStreamer
  color handling is typically gamma-encoded per `GstVideoInfo`/colorimetry, with HDR support split
  across `gst-plugins-bad` and custom elements. Match correctness and HDR behaviour (HLG/PQ,
  BT.2390) is **unproven** on GES. **Spike update:** not exercised; still open.
- **Scopes compute pass.** Concat counts the monitor canvas in a compute pass as it is drawn
  (`concat/<crate>/src/scopes.rs`), read back without blocking (`take_scope`, polled). GES offers no
  analogous non-blocking GPU hook; a readback would stall, and a custom element would be required.
  **Spike update:** `glshader` is fragment-only; a compute pass needs `glcompute` or a custom
  element. Still open.
- **`FramePlan` as a contract.** Its value is backend independence (CPU oracle vs GPU, SSIM parity
  > 0.99). Under GES there is no second compositor to hold parity against; the contract becomes the
  > host→GES adapter shape, if it survives at all.

**Consequence.** Either Genesis-0 accepts GES's compositor and **re-authors effects** against
GStreamer primitives, or it builds a custom RHI compositor fed by decoded frames — which contradicts
"GES is the engine" and duplicates work GES already does. This fork is the **Phase 0 gate**. The
`docs/architecture.md` §6 line "wgpu/Skia only if a custom compositor is ever needed; not selected"
was written before this rewrite; Concat's compositor may force that selection, and doing so must be an
explicit product/engineering decision, not a silent drift.

**Gate update (2026-09-29): PASSED.** The Phase 0 spike proved the zero-copy GL handoff of GES output
into the Qt Quick scene graph (GPU-resident `GLMemory`, single texture, no CPU readback). The verdict
is recorded in `docs/decisions/compositor-path.md`: **keep GES's compositor**; a custom RHI compositor
is not selected. The fork above is therefore closed for the render path, and the **`FramePlan` adapter
boundary is not required**; the host model → GES graph compilation (Phase 2) still is. The original
risk text above is kept: it records what was genuinely uncertain before the spike.

### (b) 123 effect packages (WGSL) vs the Extension/Pack model (R7/R8) — HIGH RISK

Concat's packages are directories: `effect.toml` (`format = 2`) + `effect.wgsl` for picture effects,
or an FFmpeg chain template for sound filters. 98 WGSL shaders / 123 manifests exist; all manifests in
the checkout are format 2. The pipeline validates WGSL with **naga**, compiles per-package preludes,
lays out `Params` uniform buffers, refuses extra bindings and unbounded loops, bounds LUT size, and
trials each shader once at 512 px under a three-second timeout.

Genesis-0's model is different in kind:

- The **Extension** is executable third-party logic under a sandbox (T3, web mini-app vs WASM, not yet
  chosen); the **Pack** is a declarative preset/asset bundle, and R8 says presets alone do **not**
  satisfy the ecosystem goal.
- A Concat picture package is _declarative manifest + host-executed shader_, closer to a **Pack** than
  to an Extension: the shader runs inside the host's pipeline and cannot do arbitrary IO. That is a
  legitimate Pack-shaped surface, but it is not the executable Extension R8 demands, and it does not
  give a path for the 123 packages to become Extensions.
- **WGSL → host execution.** Genesis-0 has no selected WGSL execution path. Translating WGSL to GLSL
  for `glshader` loses the multi-pass graph; running WGSL via a custom RHI pipeline re-opens §3(a).
  naga has no C++ equivalent chosen; validation would be re-derived.

### (c) Snapshot undo vs command-stack undo

Concat's `Editor` (`concat/<crate>/src/editor.rs`) keeps whole-state snapshots with an undo depth of
**200**; `Arc`-sharing makes a snapshot copy pointers and only the touched timeline's clips. Two
behaviors are baked in: **gestures coalesce** into one undo step (a knob drag commits on each pause),
and **view state is not an edit**. HDR auto-promotion (`follow_first_hdr`) happens _inside the command
that causes it_, so it is undone as part of that step.

Genesis-0's architecture §7 specifies **commands over the domain model** (command/redo stack), not
snapshots. Re-deriving Concat's exact undo semantics as commands is feasible but subtle:

- Gesture coalescing, view-state exclusion, and "clamps live in one place" (`Clip::blank`/`Clip::tidy`
  in Concat) must each be reproduced; an inverse-command model can diverge from snapshot restore on
  floating-point-free rational edits less easily, but on _derived_ state (id minting, key
  re-anchoring) it can.
- The acceptance bar is "undo/redo restores graph state exactly" (`docs/implementation-plan.md` Phase
  2). Snapshot-equivalent behavior must be proven, including the coupled HDR step.
- Depth semantics (200) and gesture granularity are product decisions Genesis-0 must restate, not
  inherit silently.

### (d) `f64`-seconds document vs rational-time host model

Concat freezes `f64` seconds in the document because "the documents that exist freeze the format"
(`concat/<crate>/src/model.rs`), and converts to exact `Rational` only at the render boundary
(`concat-export`'s `resolve`), quantised to the frame grid. Keyframes are **fractions of clip length**
and are re-anchored through split/trim/freeze/merge to keep the document at version 1.

Genesis-0 stores **rational time everywhere; no float seconds for edit points** (architecture §7).
Consequences:

- The document schema and all edits change representation; equality and rounding behavior must be
  re-proven frame-by-frame against the frame grid.
- Keyframe fraction re-anchoring must be re-derived over rationals; this is a correctness hotspot
  (the "keys stay on their instant of the picture" invariant).
- **Project compatibility is an open question** (§7 Q3): does Genesis-0 need to read `concat.json`
  (a one-way migration), or is it a new format with no Concat file compatibility?

### (e) Slint UI vs Qt Quick/QML

Concat's window is one Slint tree published from Rust (`concat/<crate>/src/studio.rs`, `panes/`), with
pane state/message/update/data, an _echo_ clone mutated by gestures and committed as one command, row
models diffed by `sync`, and a monitor that draws on the window's own wgpu device so a frame is a
texture Slint samples with **no readback**. It has a desktop shape, a phone shell (`ui/phone/`), and
an i18n inventory.

None of this is portable to QML. Re-derivation load is high because the UI is the largest single body
of source (27.7k Slint lines).

- **Preview presentation** (`docs/architecture.md` §5, T4): **proven (2026-09-29)** — the Phase 0
  spike renders a GES pipeline into a Qt Quick scene zero-copy, in Qt's own GL context
  (`docs/decisions/compositor-path.md`). This risk is closed; the UI re-derivation itself remains.
- **Gestures/echo** must be re-derived; QML's model/view and worker-thread boundaries (R4) do not
  match Slint's publish/diff scheme.
- **Phone shell** behavior is re-derived on Qt for mobile, not ported from `ui/phone/`.

### (f) Additional risks (beyond the required list)

- **Audio.** Concat decodes per-clip spans to memory-mapped WAVs and mixes in-process
  (`concat/<crate>/src/playback.rs`). GES owns audio decode/mixing; preview/export audio consistency
  must be re-established, and the "proxies never leak into export" contract (architecture §8) must be
  re-proven on GES.
- **Proxy/scheduler/hardware decode.** Concat's one-per-process prefetcher, source/treated caches,
  and per-platform hardware decode preference (`hardware.rs`) are process-wide policy. GES's own
  scheduling and hardware paths differ; the _behaviors_ (monitor-first priority, bounded decoders,
  proxy adoption) must be re-expressed as host policy, not assumed.
- **Mobile.** Nothing transfers: Concat's Slint Android host and its `crates/concat-android` activity
  are dropped; the mobile path is Qt + native adapters, untested.
- **Speech/vision runtimes.** whisper.cpp, Kokoro, chatterbox, sherpa-onnx, and `ort` (ONNX Runtime
  rc) are Rust-side choices; C++ equivalents are **not selected**.
- **HDR deep decode.** Concat's HDR path assumes a GPU upload-time conversion; on GES, decode is the
  engine's, so the deep/decode options (`DecodeOptions::deep`) have no direct GES control surface.

## 4. Ordered rewrite phases

Ordering: **Phase 0 gates everything.** Each phase lists scope, planned artifacts (future paths),
dependencies, verification owner role, acceptance tests, and stop/fallback. Phases are deliberately
coarser than `docs/implementation-plan.md`; where they align it is noted.

### Phase 0 — GES render-into-Qt-Quick + compositor capability spike (GATE)

**Status: PASSED (2026-09-29).** Both halves are confirmed: GES builds a timeline from a host model
and renders end-to-end, and GES output presents inside a Qt6 Quick window via RHI with zero CPU
readback. Verdict: `docs/decisions/compositor-path.md`; caps and API constraints:
`docs/dependencies.md` §"Phase 0 spike result".

Confirmed:

- GES builds a timeline from a host model (`ges_layer_add_asset`), correct durations, renders
  end-to-end (`ges-launch-1.0` → 2.03 s VP8 file).
- Chained `glshader` passes compose, and frames stay GPU-resident as `GLMemory` with
  `texture-target=2D` — the caps Qt Quick needs.
- GES output presents **inside a Qt6 Quick window via RHI** (the `spikes/ges-qt-render/` half),
  GPU-resident with no CPU readback, on a real Wayland display.

Still open (render-layer items, separate from the compositor verdict — see
`docs/decisions/compositor-path.md` §5):

- Named-intermediate multi-pass graphs (a linear chain is proven; a graph is not).
- Extended-linear half-float working space; non-blocking scopes compute pass.

**Scope.** Prove (or disprove) the central hypotheses: GES output presents inside Qt Quick via RHI
without an unconditional readback; GES's compositor can express the required effect/colour behavior,
or a justified custom-compositor decision is recorded.

**Planned artifacts**

- `spikes/ges-qt-render/` (`CMakeLists.txt`, `main.cpp`, a QML item consuming a GES-provided texture)
- `spikes/ges-effect-probe/` (multi-pass shader feasibility, linear-light working space, blend modes,
  scopes readback cost)
- `docs/decisions/compositor-path.md` (GES compositor vs custom RHI compositor vs MLT fallback, with
  the evidence)

**Dependencies.** Installed 2026-09-28 with explicit user permission (`cmake` 4.4.3, `ninja` 1.13.2,
`gst-editing-services` 1.28.6, `gst-plugins-good`/`bad`/`libav` 1.28.6) — see `docs/dependencies.md`.
**Owner:** engine spike lead + Qt/RHI lead.

**Acceptance tests**

- A GES pipeline renders visible motion inside a Qt6 Quick window on this Linux dev host; the
  frame path is described (no readback vs readback), **without** any performance claim.
- The effect probe records, concretely: whether named intermediate textures are expressible (**yes —
  answered 2026-09-29: not with stock elements, but a `GstGLMixer` subclass binds N inputs into one
  shader; `spikes/gl-custom-element/`); whether linear-light half-float compositing is available;
  whether a non-blocking scopes readback exists.
- `docs/decisions/compositor-path.md` records the verdict with reproducible commands.

**Stop/fallback.** If GES cannot present into Qt Quick acceptably, escalate per
`docs/decisions/engine-choice.md`: MLT desktop-only, or an explicit custom-RHI-compositor decision.
**Do not** silently pick a path; **do not** start Phase 1+.

### Phase 1 — Host timeline/project core

**Scope.** Host-owned, engine-agnostic C++23 model: rational time, document model, editing commands,
command-stack undo, serialization/migrations, capability records. No engine linkage.

**Planned artifacts**

- `core/time/` (rational arithmetic, frame grid)
- `core/project/` (document model, schema, migrations)
- `core/undo/` (command stack, gestures, view-state exclusion)
- `core/effects/` (effect graph + control schema types)
- `tests/core/` (unit + round-trip + undo-exactness)

**Dependencies.** Phase 0 gate passed. **Owner:** core lead.

**Acceptance tests**

- Rational edit points round-trip through serialization; no float seconds at edit points.
- Undo/redo restores graph state exactly, including a coupled HDR-promotion step and gesture
  coalescing; view-state changes are excluded.
- Adapter contract is expressed as interface only; **no GES/MLT type appears** (R6).

**Stop/fallback.** If exact-undo under commands cannot hold for a given edit, keep that edit
snapshot-equivalent behind the same interface and record why; never expose engine types.

**Status (2026-09-29).** Host core ported from `concat-project` and verified. Artifacts live at
`src/core/` and `src/project/` (flat, not the `core/time/`-style paths sketched above). Build:
`cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build` — green.

- **Acceptance 1 (rational edit points).** Met. Eight time-valued fields migrated `double` →
  `Rational` (`Clip::start`/`duration`/`source_start`/`fade_in`/`fade_out`, `Transition::duration`,
  `Stroke::at`, `MediaItem::duration`), and the document layer round-trips them: the writer emits
  exact `"num/den"` strings, the reader accepts both that and Concat's `f64` seconds (snapped once at
  import via `Rational::approximate`). `to_document` → `from_document` is exact, and a re-write is
  byte-stable.
- **Acceptance 2 (exact undo/redo).** Met and verified: `project_tests` exercises the coupled
  HDR-promotion step and gesture coalescing. Two real defects were found by the ported suite and
  fixed: redo skipped the derived steps (`tidy_touched`/`follow_first_hdr`), and coalesced-gesture
  redo replayed the gesture's first micro-move instead of its last.
- **Acceptance 3 (interface only, no engine type).** Met: the whole layer is engine-free — no Qt,
  no GStreamer, no MLT type appears.

Tallies: `core_tests` 97 / 0, `project_tests` 993 / 0, `document_tests` 134 / 0 — 1,224 checks, zero
failures, via `ctest`. The document layer follows `docs/decisions/project-file-compatibility.md`
option (b): Concat documents are **imported** tolerantly (migrations + settle) and the host writes a
native format with exact rational times. Concat's format is not written back, and unmodeled `extra`
fields are dropped by that decision.

### Phase 2 — Engine adapter (host graph → GES)

**Scope.** Compile the host graph into a GES pipeline; preserve the flatten/resolve semantics
(transitions resolved, frame-grid quantisation) without leaking engine objects (R5/R6).

**Planned artifacts**

- `adapters/engine/ges/` (builder, mapper, `FramePlan` bridge if Phase 0 kept it)
- `adapters/engine/tests/` (round-trip: host graph → pipeline → observed timeline)
- `docs/decisions/adapter-contract.md`

**Dependencies.** Phase 1. **Owner:** core lead + engine lead.

**Acceptance tests**

- Effect graph round-trips through the adapter on the tested OS.
- No codec re-implementation in the host (R5): decode/encode remain engine-side.
- Missing/unsupported effect keeps the project intact (R9).

**Stop/fallback.** Prove the adapter on **one** engine before a second is considered
(`docs/implementation-plan.md` Phase 2).

### Phase 3 — Media pipeline (decode/cache/proxy/preview)

**Scope.** Re-derive media ingestion on GES: probe, preview playback, proxies, source/treated caches
as host policy, bounded decoding, monitor-first priority.

**Planned artifacts**

- `core/media/` (asset IDs, probe/relink records)
- `adapters/engine/media/` (preview/playback, proxy generation, cache policy)
- `tests/media/` (proxy-never-in-export, seek/scan consistency)

**Dependencies.** Phase 2. **Owner:** media lead.

**Acceptance tests**

- Proxies never leak into final export (architecture §8).
- Playback/seek/timeline manipulation function on the dev host; no performance claim.
- Hardware decode/zero-copy treated as measured capability, not assumed.

**Stop/fallback.** If a behavior (e.g. per-platform hardware decode preference) has no GES control
surface, implement it as host policy or mark it unavailable; do not claim it.

### Phase 4 — Effects & Extension/Pack pipeline

**Scope.** Convert Concat's package model into Genesis-0's Extension/Pack contract and the signed
install pipeline; decide the effect execution path from Phase 0's verdict.

**Planned artifacts**

- `core/effects/catalogue/` (manifest → control schema; Pack format)
- `marketplace/schema/manifest.schema.json`, `marketplace/pipeline/`
- `plugins/sdk/` (**Extension SDK**), one independently packaged example adding new functionality
- `tests/security/hostile-packages/`
- `docs/decisions/effect-runtime.md` (WGSL strategy / GStreamer element re-authoring)

**Dependencies.** Phase 1 + Phase 0 verdict. **Owner:** effects lead + security reviewer.

**Acceptance tests**

- Manifest/control-schema contract covers Concat's `[[param]]` kinds (number, wheel, curve, color) as
  host-rendered declarative controls; arbitrary QML/JS is rejected.
- Install pipeline passes hostile-package tests (traversal, symlink, zip bomb, corrupt signature →
  rollback); updates re-consent.
- One example Extension demonstrably adds new functionality (R8); declarative presets alone rejected.
- Reused Concat shader assets retain AGPL-3.0 notices.

**Stop/fallback.** If no safe sandbox is chosen, restrict executable Extensions to the curated desktop
path; the mobile executable path stays a **gate**, not a promise.

### Phase 5 — Qt Quick/QML UI shell

**Scope.** Adaptive desktop workspace + preview; re-derive Concat's pane/controller behavior in QML.

**Planned artifacts**

- `ui/qml/` (tokens/components, workspace, timeline, inspector, monitor)
- `ui/qml/models/` (virtualized thumbnails/waveforms, R4)
- `ui/i18n/` (Qt `.ts` inventory re-derived from Concat's inventory)

**Dependencies.** Phases 1–4. **Owner:** UI lead.

**Acceptance tests**

- No media work on the UI thread; thumbnails/waveforms/proxies virtualized (R4).
- Breakpoints use width **and** height/input mode (landscape phone not misclassified) (R2).
- Touch targets ~44–48 units; focus, reduced motion, safe areas (R3).

**Stop/fallback.** If monitor presentation fails on a target, gate the feature by platform rather
than claiming parity.

### Phase 6 — Speech/vision/AI job contract

**Scope.** Async job contract (submit/status/progress/cancel/result) with at least one provider path;
masks/tracking and transcription features re-derived.

**Planned artifacts**

- `core/jobs/`, `ai/providers/`, `core/masks/`, `core/tracking/`
- `docs/decisions/ai-provider.md`

**Dependencies.** Phase 1. **Owner:** AI integration lead.

**Acceptance tests**

- Generation adds nondestructive clips; analysis adds masks/tracks/captions; source never overwritten
  (R10).
- Cancellation and crash recovery verified; pausing does not free VRAM.

### Phase 7 — API / CLI / remote surface

**Scope.** Re-derive Concat's verb-as-JSON command surface as the host command/API layer; decide
whether a remote server (Concat's `concat-server`) is in product scope.

**Planned artifacts**

- `core/api/` (verb dispatcher over the command model)
- `tools/cli/` (thin client) — only if chosen (see §7 Q7)

**Dependencies.** Phase 1. **Owner:** core lead.

**Acceptance tests.** Every command usable by the UI is reachable through the API with an identical
result; no engine type crosses the boundary.

**Stop/fallback.** If a remote server is not in scope, the API stays in-process and the server crate
stays dropped.

### Phase 8 — Mobile native adapters

**Scope.** Qt for Android/iOS UI + native engine adapters (AVFoundation/Media3) per
`docs/decisions/engine-choice.md` §4.

**Planned artifacts**

- `adapters/mobile/ios/`, `adapters/mobile/android/`
- `ui/qml/mobile/`
- `docs/decisions/mobile-engine-mapping.md`

**Dependencies.** Phases 1, 5. **Owner:** mobile lead.

**Acceptance tests**

- Portable project opens on real Android/iOS hardware (no fake iOS tests on Linux).
- Store eligibility treated as a gate (Apple 4.7, Play downloaded-code rules) [S5][S6].

**Stop/fallback.** If the mobile engine path cannot meet the project contract, keep the platform
functional and explicit rather than claiming parity.

### Phase 9 — Release hardening

**Scope.** Packaging, CI matrix (five OS families), signed releases, device perf on fixtures, legal.

**Dependencies.** Phases 0–8. **Owner:** release engineer + security + legal.

**Stop/fallback.** Per-platform capability notes instead of a universal-performance claim.

## 5. Effort sizing

Order-of-magnitude only. The source is **~114k lines** (84,370 Rust + 27,743 Slint + 1,744 WGSL), but
the rewrite is **not** a 1:1 translation: the engine, compositor, and UI substrates are replaced.

**Assumptions (state-changing, not conclusions):**

1. "Parity" means Concat's _shipped feature set_ re-derived on GES/Qt at comparable behavior, not
   line-for-line equivalence.
2. Re-derivation cost per line is lower than greenfield for logic (core/project/export), higher than
   a translation for replace-verdict code (compositor/effects/UI), because behavior is re-discovered
   from tests and the spec.
3. Phase 0's verdict can invalidate Phases 2/4 estimates; the custom-compositor branch is materially
   more expensive than the GES-compositor branch.
4. One experienced engineer-month ≈ one competent contributor, assuming the toolchain works.

| Phase                                | Order-of-magnitude                                         |
| ------------------------------------ | ---------------------------------------------------------- |
| 0 — GES/Qt render + compositor spike | weeks, not months                                          |
| 1 — Host timeline/project core       | low single-digit engineer-months                           |
| 2 — Engine adapter                   | low single-digit engineer-months                           |
| 3 — Media pipeline                   | low-to-mid single-digit engineer-months                    |
| 4 — Effects/Extension pipeline       | mid single-digit engineer-months; highest uncertainty      |
| 5 — Qt Quick/QML UI                  | high single-digit engineer-months (largest body of source) |
| 6 — Speech/vision/AI contract        | low single-digit engineer-months (provider-dependent)      |
| 7 — API/CLI                          | low single-digit engineer-months                           |
| 8 — Mobile adapters                  | mid-to-high single-digit engineer-months                   |
| 9 — Release hardening                | low-to-mid single-digit engineer-months                    |

**Aggregate (order of magnitude):** desktop parity is a **multi-engineer-year** effort; full
five-platform scope is larger still. These are **planning inputs, not commitments**;
`docs/architecture.md` deliberately disclaims effort estimates and this table does not overturn that.

## 6. What to do first

**The Phase 0 gate has passed (2026-09-29).** The toolchain is installed (2026-09-28) and both spike
halves are confirmed; the compositor verdict is recorded in `docs/decisions/compositor-path.md`.

1. ~~Obtain explicit user permission to install the missing packages~~ **Done 2026-09-28** —
   `cmake` 4.4.3, `ninja` 1.13.2, `gst-editing-services` 1.28.6, `gst-plugins-good`/`bad`/`libav`
   1.28.6. See `docs/dependencies.md`.
2. ~~Build `spikes/ges-qt-render/`~~ **Done 2026-09-29** — GES output presents inside a Qt6 Quick
   window via RHI, GPU-resident with no CPU readback. See `docs/decisions/compositor-path.md`.
3. ~~**Run `spikes/ges-effect-probe/`**: two shader-like effects with an intermediate picture and a
   linear-light blend, plus one non-blocking readback probe, to answer §3(a) concretely.~~ **Partly
   done 2026-09-29** — `spikes/gl-multipass/` and `spikes/gl-custom-element/` settled the
   multi-pass question (stock elements cannot express named intermediates; a custom `GstGLMixer`
   subclass can — pixel-verified). **Still unproven: the linear-light half-float working space and
   the non-blocking scopes readback.**
4. ~~Record `docs/decisions/compositor-path.md`~~ **Done 2026-09-29** — verdict: keep GES's
   compositor. Everything after Phase 0 depends on this decision.

The compositor gate (§3(a)) is closed by `docs/decisions/compositor-path.md`, so the host core may
proceed (it has: see the Phase 1 status below). The effect-model question (§3(b)) still gates the
effect execution path (Phase 4).

## 7. Open questions

1. **Crate count.** The brief says 17 crates; the checkout has 16 workspace members (this document
   maps all 16). Name the 17th if one is intended, or correct the count.
2. **Fidelity target.** Exact behavioral parity with Concat (including `Clip::tidy` clamps, gesture
   granularity, HDR auto-promotion), or "Concat-class features" with Genesis-0 rules? This changes
   Phase 1 and the acceptance tests.
3. **Project-file compatibility.** Must Genesis-0 read/migrate existing `concat.json` documents
   (one-way migration), or is this a new format with no Concat file compatibility? (§3(d).)
4. **Effect scope.** Must all 123 packages be re-derived, or a curated subset? The WGSL multi-pass
   model has no GES equivalent (§3(b)); re-authoring all of them against GStreamer elements is a
   large, separate effort.
5. **Compositor decision.** **Resolved (2026-09-29).** The Phase 0 spike passed; the decision is to
   keep GES's compositor, recorded in `docs/decisions/compositor-path.md`. A custom RHI compositor
   (overturning "not selected" in architecture §6) is the escape hatch only if a future need demands
   it — multi-stream mixing inside Qt, per-layer effects GES cannot express, or the `FramePlan`
   contract proving necessary as an adapter boundary.
6. **AI providers.** Which C++ speech/vision providers replace whisper.cpp/Kokoro/`ort`, and is
   transcription/TTS in the first parity target or deferred?
7. **API/remote scope.** Is Concat's `concat-server` (JSON-RPC/gRPC/token server) in product scope, or
   does Genesis-0 stay in-process with the API layer only?
8. **Mobile engine mapping.** Confirm the native-adapter plan (`docs/decisions/engine-choice.md` §4)
   is the mobile path and that the desktop GES engine is not attempted on mobile.
9. **Effort/resourcing.** Who owns each verification-owner role, and is the multi-year aggregate in
   §5 accepted before Phase 1 begins?

---

## Appendix — measured Concat figures used above

| Measure           | Value                                 | Source                                             |
| ----------------- | ------------------------------------- | -------------------------------------------------- |
| Rust              | 84,370 lines / 132 files              | checkout scan                                      |
| Slint UI          | 27,743 lines / 63 files               | checkout scan                                      |
| WGSL              | 1,744 lines / 98 files                | checkout scan                                      |
| Effect packages   | 123 (`effect.toml`, all `format = 2`) | `concat/src/crates/concat-effects/packages/`       |
| Workspace members | 16                                    | `concat/src/Cargo.toml` (`members = ["crates/*"]`) |

Per-crate Rust LOC: `concat` 19,635; `concat-host` 11,540; `concat-media` 9,386; `concat-project`
9,192; `concat-render` 8,044; `concat-effects` 7,090; `concat-export` 3,498; `concat-speech` 3,094;
`concat-core` 2,829; `concat-vision` 2,760; `concat-api` 2,350; `concat-text` 1,635; `concat-server`
1,376; `concat-perf` 1,092; `concat-cli` 514; `concat-android` 335.
