# Rewrite and Command-Layer Plan (Historical Record)

> Status: historical. These plans were executed. They are a record of intent,
> not a description of the current tree. Do not read as current design.

## A. Reference-to-Genesis rewrite   (was docs/history/rewrite.md)

---

## 0. How to read this plan

- Section 1 fixes what "the rewrite" means and what it does not.
- Section 2 maps each measured reference crate to a Genesis-0 module and a verdict.
- Section 3 is the load-bearing section: where the reference editor's architecture does **not** map onto GES/Qt.
- Section 4 orders the work; **Phase 0 gates every later phase**.
- Sections 5–7 are sizing, the immediate next step, and decisions the user must make.

The reference editor's size, measured from the checkout (the reference checkout's `src/crates/*`): **84,370 Rust lines** across 132
files, **27,743 Slint lines** across 63 files, **1,744 WGSL lines** across 98 files, and **123 effect
packages** under the reference `effects` crate's `packages/`. A workspace scan finds **16 crates**,
not 17 (see §2 note and §7 Q1).

## 1. Scope statement

**In scope.** Re-derive, in C++23 and Qt6 Quick/QML, the _observable behavior_ of the reference editor: its project
document and editing commands, its timeline flatten/resolve stages, its preview/export loop, its
effect catalogue and packages, its masks/vision and speech features, its API surface, and its desktop
and phone workflows — expressed through Genesis-0's host model and engine adapter, with GES as the
primary engine and native adapters on mobile.

**Explicitly not in scope.**

- **No second engine and no custom compositor by default.** `docs/architecture.md` §6 fixes GES as
  primary; `wb/wgpu`/`Skia` are listed as "only if a custom compositor is ever needed; not selected".
  Whether the reference editor's compositor forces that selection is exactly the Phase 0 gate, not a decision taken
  here.
- **No mobile implementation on the desktop engine.** Mobile is served by native adapters
  (AVFoundation/Media3) per `docs/decisions/engine-choice.md` §4; the reference editor's Android host does not
  transfer.
- **No performance, parity, or store claim.** The GES desktop render spike passed (2026-09-29,
  `docs/decisions/compositor-path.md`); mobile is untested (`docs/decisions/engine-choice.md` §2).
- **No new dependencies** (models, runtimes, libraries) are selected here; AI models and the
  sandbox runtime remain "not selected" (`docs/dependencies.md`).

"The rewrite" therefore means: **port the behavior and data concepts, replace the engine,
compositor, and UI, and keep the host portable.**

## 2. Crate → module mapping

Verdicts: **port** = re-derive logic in C++23 largely as-is; **re-derive** = same behavior, different
substrate/algorithm; **replace** = the substrate changes to Qt/GES and little code carries over;
**drop** = not carried; **defer** = planned later, not in the initial slice.

| Reference crate      | Rust LOC | Role (from the reference checkout's `ARCHITECTURE.md` §1)                                                                                           | Genesis-0 module (planned)                                    | Verdict       | Justification                                                                                                                                                                                                           |
| ----------------- | -------- | --------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- | ------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| the reference `core` crate     | 2,829    | time (`Rational`), arena, frame, timeline, shader types, retime, animate; std-only, wasm                                          | `core/time/`, `core/model/`, `core/effects/` types            | **port**      | Pure algorithms (rational time, arena, retime, animation). No IO. Maps directly onto R5's host-owned rational time (`docs/architecture.md` §7); see the reference checkout's `<crate>/src/time.rs`, `timeline.rs`, `retime.rs`, `animate.rs`. |
| the reference `project` crate  | 9,192    | document model, commands, undo, serde, migrations (`model.rs`, `commands/`, `editor.rs`, `doc.rs`)                                | `core/project/`, `core/undo/`, `core/effects/`                | **re-derive** | Document concepts port; **undo must change** from snapshot to command stack (architecture §7) — see §3(c); times must change from `f64` seconds to rational — see §3(d).                                                |
| the reference `media` crate    | 9,386    | FFmpeg probe/decode/encode/pool/prefetch/hardware (`decode.rs`, `pool.rs`, `prefetch.rs`, `hardware.rs`)                          | `adapters/engine/` (+ GES)                                    | **replace**   | The host does **not** re-implement codecs (R5); GES/GStreamer owns decode/encode. Cache/prefetch/proxy _semantics_ are re-derived on the adapter; the `ffmpeg-the-third` binding is not.                                |
| the reference `effects` crate  | 7,090    | effect packages (`effect.toml` format 2 + `effect.wgsl`), catalogue, naga validation, budgets                                     | `core/effects/` + Extension/Pack pipeline                     | **re-derive** | Manifest/param concepts map to the Extension/Pack control schema; the WGSL multi-pass execution model has no GES equivalent — see §3(b); see the reference checkout's `<crate>/src/manifest.rs`, `catalogue.rs`, `shader.rs`.                 |
| the reference `render` crate   | 8,044    | `FramePlan`, GPU compositor, scopes, SSIM (`plan.rs`, `gpu.rs`, `scopes.rs`, `reference.rs`)                                      | GES compositor + `adapters/engine/` FramePlan bridge          | **replace**   | The custom wgpu compositor is displaced by GES's compositor; whether the `FramePlan` contract can be preserved as an adapter boundary is a Phase 0 question — see §3(a).                                                |
| the reference `vision` crate   | 2,760    | cutout masks, brushes, enhance (ONNX) (`mask.rs`, `brush.rs`, `enhance.rs`)                                                       | `core/masks/`, `core/tracking/`, AI job runtime               | **defer**     | ONNX Runtime is not selected; masks/rotoscoping are Phase 3 of `docs/history/implementation.md`. Behavior re-derived later; the `ort` dependency is not carried.                                                           |
| the reference `text` crate     | 1,635    | title rasteriser                                                                                                                  | Qt text rendering (`QTextDocument`/`QPainter`) or Pack        | **replace**   | Qt owns text/font layout/rasterisation; the Slint-era rasteriser is not portable.                                                                                                                                       |
| the reference `export` crate   | 3,498    | document → engine timeline, render loop, preview (`flatten.rs`, `resolve.rs`, `lib.rs`)                                           | `adapters/engine/`                                            | **re-derive** | This is the core of the port: flatten (`Vec<ExportClip>`) and resolve (rational `Timeline`) become the adapter that compiles the host graph into a GES pipeline. Behavior preserved; GES types never leak (R6).         |
| the reference `host` crate     | 11,540   | Session, projects, playback, monitor, proxies, scheduler, jobs (`session.rs`, `playback.rs`, `preview.rs`, `proxy.rs`, `jobs.rs`) | `core/` services + `adapters/` + `core/jobs/`                 | **re-derive** | Session/proxy/job _semantics_ port to the host; playback/monitor/scheduler are re-implemented on GES + Qt worker threads (R4: no media work on the UI thread).                                                          |
| the reference `speech` crate   | 3,094    | whisper.cpp transcription, Kokoro TTS, chatterbox (`transcribe.rs`, `tts.rs`)                                                     | `ai/providers/` (AI job contract)                             | **defer**     | C++ providers/models are not selected (`docs/architecture.md` §12). The job contract (submit/status/progress/cancel/result) ports; the Rust model crates do not.                                                        |
| the reference `api` crate      | 2,350    | one dispatcher, every verb as JSON (`message.rs`, `lib.rs`)                                                                       | host command/API layer                                        | **re-derive** | The reference editor's "every verb as JSON" aligns naturally with Genesis-0's command model; re-derive as the host command surface. Optional remote exposure is §7 Q7.                                                                |
| the reference `server` crate   | 1,376    | JSON-RPC lines, gRPC, tokens (`lib.rs`, `json.rs`, `grpc.rs`, `token.rs`)                                                         | — (out of initial scope)                                      | **drop**      | Not required by any Genesis-0 decision register entry; a remote server with gRPC and token minting is product scope not yet chosen.                                                                                     |
| the reference `cli` crate      | 514      | command-line driver                                                                                                               | — (defer to tooling)                                          | **defer**     | A thin client over the API; revisit after the API layer exists.                                                                                                                                                         |
| the reference `window` crate | 19,635   | Slint window, panes, studio controller, monitor (`studio.rs`, `panes/`, `gpu.rs`)                                                 | `ui/qml/`                                                     | **replace**   | 27.7k Slint lines are not portable; the pane state/message model is re-derived in QML/C++. Monitor texture integration is the Phase 0 gate — see §3(e).                                                                 |
| the reference `android` crate  | 335      | Android activity hosting the Slint window                                                                                         | native mobile adapters (`docs/decisions/engine-choice.md` §4) | **drop**      | Slint's `android-activity` backend is specific to the reference editor; Genesis-0 mobile is Qt + Media3/AVFoundation.                                                                                                                 |
| the reference `perf` crate     | 1,092    | performance table (`main.rs`)                                                                                                     | `benchmarks/`                                                 | **re-derive** | Budgets and scenarios are re-derived as benchmarks against the fixtures of `docs/history/implementation.md` Phase 0; no reference figure is inherited.                                                                        |

**Note on the crate count.** The brief states 17 crates and 16 per-crate LOC figures; the checkout's
workspace (the reference checkout's `src/Cargo.toml`, `members = ["crates/*"]`) contains **16** members and the mapping
table above covers all 16. If a 17th crate is intended, it must be named (§7 Q1); no such member is
present in the read-only checkout.

## 3. Feasibility risks

This section is the point of the document. Each item is where the reference editor's architecture **does not** map
onto GES/Qt, with the least-optimistic reading.

### (a) Custom `FramePlan` + wgpu compositor vs GES's compositor — HIGHEST RISK

The reference editor's renderer is a first-class custom pipeline: `plan_frame(t)` produces a pure `FramePlan`
(the reference checkout's `<crate>/src/plan.rs:434`) describing visible layers, source timestamps, placement, opacity,
blend and per-layer treatments; the executor fills it with decoded pictures and resolved effects; a
single `WgpuCompositor` draws every frame (monitor and export) with a working space of **linear light,
Rec.709 primaries, extended scRGB, half floats**, converting on upload (`COPY_SHADER`, `DEEP_SHADER`)
and rolling off HDR with BT.2390.

GES composites through GStreamer's `compositor`/`videoaggregator` and `gnlcomposition`, not through a
programmable `FramePlan`. The mismatches are specific:

- **Multi-pass WGSL effects.** A format-2 package declares named passes (`[[wgsl.pass]] target=...,
shrink=[...]`, e.g. the reference `gaussian-blur` pack's `effect.toml`), intermediate pictures, and `<target>_at`
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
  the look only when every stage reads the same source. See `docs/decisions/effect-execution.md` §6.
- **Working space.** GES does not expose an extended-linear half-float compositing space; GStreamer
  color handling is typically gamma-encoded per `GstVideoInfo`/colorimetry, with HDR support split
  across `gst-plugins-bad` and custom elements. Match correctness and HDR behaviour (HLG/PQ,
  BT.2390) is **unproven** on GES. **Spike update:** not exercised; still open.
- **Scopes compute pass.** The reference editor counts the monitor canvas in a compute pass as it is drawn
  (the reference checkout's `<crate>/src/scopes.rs`), read back without blocking (`take_scope`, polled). GES offers no
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
was written before this rewrite; the reference editor's compositor may force that selection, and doing so must be an
explicit product/engineering decision, not a silent drift.

**Gate update (2026-09-29): PASSED.** The Phase 0 spike proved the zero-copy GL handoff of GES output
into the Qt Quick scene graph (GPU-resident `GLMemory`, single texture, no CPU readback). The verdict
is recorded in `docs/decisions/compositor-path.md`: **keep GES's compositor**; a custom RHI compositor
is not selected. The fork above is therefore closed for the render path, and the **`FramePlan` adapter
boundary is not required**; the host model → GES graph compilation (Phase 2) still is. The original
risk text above is kept: it records what was genuinely uncertain before the spike.

### (b) 123 effect packages (WGSL) vs the Extension/Pack model (R7/R8) — HIGH RISK

The reference editor's packages are directories: `effect.toml` (`format = 2`) + `effect.wgsl` for picture effects,
or an FFmpeg chain template for sound filters. 98 WGSL shaders / 123 manifests exist; all manifests in
the checkout are format 2. The pipeline validates WGSL with **naga**, compiles per-package preludes,
lays out `Params` uniform buffers, refuses extra bindings and unbounded loops, bounds LUT size, and
trials each shader once at 512 px under a three-second timeout.

Genesis-0's model is different in kind:

- The **Extension** is executable third-party logic under a sandbox (T3, web mini-app vs WASM, not yet
  chosen); the **Pack** is a declarative preset/asset bundle, and R8 says presets alone do **not**
  satisfy the ecosystem goal.
- A reference picture package is _declarative manifest + host-executed shader_, closer to a **Pack** than
  to an Extension: the shader runs inside the host's pipeline and cannot do arbitrary IO. That is a
  legitimate Pack-shaped surface, but it is not the executable Extension R8 demands, and it does not
  give a path for the 123 packages to become Extensions.
- **WGSL → host execution.** Genesis-0 has no selected WGSL execution path. Translating WGSL to GLSL
  for `glshader` loses the multi-pass graph; running WGSL via a custom RHI pipeline re-opens §3(a).
  naga has no C++ equivalent chosen; validation would be re-derived.

### (c) Snapshot undo vs command-stack undo

The reference editor's `Editor` (the reference checkout's `<crate>/src/editor.rs`) keeps whole-state snapshots with an undo depth of
**200**; `Arc`-sharing makes a snapshot copy pointers and only the touched timeline's clips. Two
behaviors are baked in: **gestures coalesce** into one undo step (a knob drag commits on each pause),
and **view state is not an edit**. HDR auto-promotion (`follow_first_hdr`) happens _inside the command
that causes it_, so it is undone as part of that step.

Genesis-0's architecture §7 specifies **commands over the domain model** (command/redo stack), not
snapshots. Re-deriving the reference editor's exact undo semantics as commands is feasible but subtle:

- Gesture coalescing, view-state exclusion, and "clamps live in one place" (`Clip::blank`/`Clip::tidy`
  in the reference editor) must each be reproduced; an inverse-command model can diverge from snapshot restore on
  floating-point-free rational edits less easily, but on _derived_ state (id minting, key
  re-anchoring) it can.
- The acceptance bar is "undo/redo restores graph state exactly" (`docs/history/implementation.md` Phase
  2). Snapshot-equivalent behavior must be proven, including the coupled HDR step.
- Depth semantics (200) and gesture granularity are product decisions Genesis-0 must restate, not
  inherit silently.

### (d) `f64`-seconds document vs rational-time host model

The reference editor freezes `f64` seconds in the document because "the documents that exist freeze the format"
(the reference checkout's `<crate>/src/model.rs`), and converts to exact `Rational` only at the render boundary
(the reference `export` crate's `resolve`), quantised to the frame grid. Keyframes are **fractions of clip length**
and are re-anchored through split/trim/freeze/merge to keep the document at version 1.

Genesis-0 stores **rational time everywhere; no float seconds for edit points** (architecture §7).
Consequences:

- The document schema and all edits change representation; equality and rounding behavior must be
  re-proven frame-by-frame against the frame grid.
- Keyframe fraction re-anchoring must be re-derived over rationals; this is a correctness hotspot
  (the "keys stay on their instant of the picture" invariant).
- **Project compatibility** (§7 Q3, resolved): a host-native format with no legacy file
  compatibility — the reader accepts only the current shape.

### (e) Slint UI vs Qt Quick/QML

The reference editor's window is one Slint tree published from Rust (the reference checkout's `<crate>/src/studio.rs`, `panes/`), with
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

- **Audio.** The reference editor decodes per-clip spans to memory-mapped WAVs and mixes in-process
  (the reference checkout's `<crate>/src/playback.rs`). GES owns audio decode/mixing; preview/export audio consistency
  must be re-established, and the "proxies never leak into export" contract (architecture §8) must be
  re-proven on GES.
- **Proxy/scheduler/hardware decode.** The reference editor's one-per-process prefetcher, source/treated caches,
  and per-platform hardware decode preference (`hardware.rs`) are process-wide policy. GES's own
  scheduling and hardware paths differ; the _behaviors_ (monitor-first priority, bounded decoders,
  proxy adoption) must be re-expressed as host policy, not assumed.
- **Mobile.** Nothing transfers: the reference editor's Slint Android host and the reference `android` crate activity
  are dropped; the mobile path is Qt + native adapters, untested.
- **Speech/vision runtimes.** whisper.cpp, Kokoro, chatterbox, sherpa-onnx, and `ort` (ONNX Runtime
  rc) are Rust-side choices; C++ equivalents are **not selected**.
- **HDR deep decode.** The reference editor's HDR path assumes a GPU upload-time conversion; on GES, decode is the
  engine's, so the deep/decode options (`DecodeOptions::deep`) have no direct GES control surface.

## 4. Ordered rewrite phases

Ordering: **Phase 0 gates everything.** Each phase lists scope, planned artifacts (future paths),
dependencies, verification owner role, acceptance tests, and stop/fallback. Phases are deliberately
coarser than `docs/history/implementation.md`; where they align it is noted.

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
- `docs/decisions/compositor-path.md` (GES compositor vs custom RHI compositor, with
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
`docs/decisions/engine-choice.md`: an explicit custom-RHI-compositor decision.
**Do not** silently pick a path; **do not** start Phase 1+.

### Phase 1 — Host timeline/project core

**Scope.** Host-owned, engine-agnostic C++23 model: rational time, document model, editing commands,
command-stack undo, serialization/versioning. No engine linkage.

**Planned artifacts**

- `core/time/` (rational arithmetic, frame grid)
- `core/project/` (document model, schema)
- `core/undo/` (command stack, gestures, view-state exclusion)
- `core/effects/` (effect graph + control schema types)
- `tests/core/` (unit + round-trip + undo-exactness)

**Dependencies.** Phase 0 gate passed. **Owner:** core lead.

**Acceptance tests**

- Rational edit points round-trip through serialization; no float seconds at edit points.
- Undo/redo restores graph state exactly, including a coupled HDR-promotion step and gesture
  coalescing; view-state changes are excluded.
- Adapter contract is expressed as interface only; **no GES type appears** (R6).

**Stop/fallback.** If exact-undo under commands cannot hold for a given edit, keep that edit
snapshot-equivalent behind the same interface and record why; never expose engine types.

**Status (2026-09-29).** Host core ported from the reference `project` crate and verified. Artifacts live at
`src/core/` and `src/project/` (flat, not the `core/time/`-style paths sketched above). Build:
`cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build` — green.

- **Acceptance 1 (rational edit points).** Met. Eight time-valued fields migrated `double` →
  `Rational` (`Clip::start`/`duration`/`source_start`/`fade_in`/`fade_out`, `Transition::duration`,
  `Stroke::at`, `MediaItem::duration`), and the document layer round-trips them: the writer emits
  exact `"num/den"` strings, and the reader accepts only that string form (a JSON number is not read).
  `to_document` → `from_document` is exact, and a re-write is
  byte-stable.
- **Acceptance 2 (exact undo/redo).** Met and verified: `project_tests` exercises the coupled
  HDR-promotion step and gesture coalescing. Two real defects were found by the ported suite and
  fixed: redo skipped the derived steps (`tidy_touched`/`follow_first_hdr`), and coalesced-gesture
  redo replayed the gesture's first micro-move instead of its last.
- **Acceptance 3 (interface only, no engine type).** Met: the whole layer is engine-free — no Qt,
  no GStreamer, no engine type appears.

Tallies: `core_tests` 97 / 0, `project_tests` 993 / 0, `document_tests` 134 / 0 — 1,224 checks, zero
failures, via `ctest`. The document layer follows `docs/decisions/project-file-format.md`:
it is a host-native format with exact rational times and no legacy-document compatibility — no
migrations, no flat-timeline lifting, no `f64`-seconds edit points. The reference editor's format is
neither read nor written back.

### Phase 2 — Engine adapter (host graph → GES)

**Scope.** Compile the host graph into a GES pipeline; preserve the flatten/resolve semantics
(transitions resolved, frame-grid quantisation) without leaking engine objects (R5/R6).

**Planned artifacts**

- `adapters/engine/ges/` (builder, mapper, `FramePlan` bridge if Phase 0 kept it)
- `adapters/engine/tests/` (round-trip: host graph → pipeline → observed timeline)
- `docs/decisions/engine-adapter-contract.md`

**Dependencies.** Phase 1. **Owner:** core lead + engine lead.

**Acceptance tests**

- Effect graph round-trips through the adapter on the tested OS.
- No codec re-implementation in the host (R5): decode/encode remain engine-side.
- Missing/unsupported effect keeps the project intact (R9).

**Stop/fallback.** Prove the adapter on **one** engine before a second is considered
(`docs/history/implementation.md` Phase 2).

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

**Scope.** Convert the reference editor's package model into Genesis-0's Extension/Pack contract and the signed
install pipeline; decide the effect execution path from Phase 0's verdict.

**Planned artifacts**

- `core/effects/catalogue/` (manifest → control schema; Pack format)
- `marketplace/schema/manifest.schema.json`, `marketplace/pipeline/`
- `plugins/sdk/` (**Extension SDK**), one independently packaged example adding new functionality
- `tests/security/hostile-packages/`
- `docs/decisions/effect-runtime.md (not created)` (WGSL strategy / GStreamer element re-authoring)

**Dependencies.** Phase 1 + Phase 0 verdict. **Owner:** effects lead + security reviewer.

**Acceptance tests**

- Manifest/control-schema contract covers the reference editor's `[[param]]` kinds (number, wheel, curve, color) as
  host-rendered declarative controls; arbitrary QML/JS is rejected.
- Install pipeline passes hostile-package tests (traversal, symlink, zip bomb, corrupt signature →
  rollback); updates re-consent.
- One example Extension demonstrably adds new functionality (R8); declarative presets alone rejected.
- Reused reference shader assets retain AGPL-3.0 notices.

**Stop/fallback.** If no safe sandbox is chosen, restrict executable Extensions to the curated desktop
path; the mobile executable path stays a **gate**, not a promise.

### Phase 5 — Qt Quick/QML UI shell

**Scope.** Adaptive desktop workspace + preview; re-derive the reference editor's pane/controller behavior in QML.

**Planned artifacts**

- `ui/qml/` (tokens/components, workspace, timeline, inspector, monitor)
- `ui/qml/models/` (virtualized thumbnails/waveforms, R4)
- `ui/i18n/` (Qt `.ts` inventory re-derived from the reference editor's inventory)

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

**Scope.** Re-derive the reference editor's verb-as-JSON command surface as the host command/API layer; decide
whether a remote server (the reference `server` crate) is in product scope.

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
- `docs/decisions/mobile-engine-mapping.md (not created)`

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

1. "Parity" means the reference editor's _shipped feature set_ re-derived on GES/Qt at comparable behavior, not
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
2. **Fidelity target.** Exact behavioral parity with the reference editor (including `Clip::tidy` clamps, gesture
   granularity, HDR auto-promotion), or "reference-class features" with Genesis-0 rules? This changes
   Phase 1 and the acceptance tests.
3. **Project-file compatibility.** **Resolved (2026-10-04).** No legacy compatibility: the host
   format is current-only — the reader accepts only the current shape (no migrations, no `f64`
   seconds) and refuses a newer `version` whole (§3(d); `docs/decisions/project-file-format.md`).
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
7. **API/remote scope.** Is the reference `server` crate (JSON-RPC/gRPC/token server) in product scope, or
   does Genesis-0 stay in-process with the API layer only?
8. **Mobile engine mapping.** Confirm the native-adapter plan (`docs/decisions/engine-choice.md` §4)
   is the mobile path and that the desktop GES engine is not attempted on mobile.
9. **Effort/resourcing.** Who owns each verification-owner role, and is the multi-year aggregate in
   §5 accepted before Phase 1 begins?

---

## Appendix — measured reference figures used above

| Measure           | Value                                 | Source                                             |
| ----------------- | ------------------------------------- | -------------------------------------------------- |
| Rust              | 84,370 lines / 132 files              | checkout scan                                      |
| Slint UI          | 27,743 lines / 63 files               | checkout scan                                      |
| WGSL              | 1,744 lines / 98 files                | checkout scan                                      |
| Effect packages   | 123 (`effect.toml`, all `format = 2`) | the reference `effects` crate's `packages/`       |
| Workspace members | 16                                    | the reference checkout's `src/Cargo.toml` (`members = ["crates/*"]`) |

Per-crate Rust LOC: the reference `window` crate 19,635; the reference `host` crate 11,540; the reference `media` crate 9,386; the reference `project` crate
9,192; the reference `render` crate 8,044; the reference `effects` crate 7,090; the reference `export` crate 3,498; the reference `speech` crate 3,094;
the reference `core` crate 2,829; the reference `vision` crate 2,760; the reference `api` crate 2,350; the reference `text` crate 1,635; the reference `server` crate
1,376; the reference `perf` crate 1,092; the reference `cli` crate 514; the reference `android` crate 335.

## B. Command and undo layer         (was docs/history/rewrite.md)

> Note: file:line citations below are as-of 2026-10-01 — they predate the model/command/document split (the original file already noted this).

---

## 0. How to read this plan

- §1 fixes the command model (C++ shapes, application, composition, ownership).
- §2 fixes undo/redo as a command stack and how a command's inverse is represented.
- §3 fixes rational time at edit points and the migration cost.
- §4 is the per-verb inventory against the reference editor's `Command` enum.
- §5 marks the serialization seam without designing it.
- §6 is the file layout; §7 verification; §8 the questions the user must answer.

Verdicts in §4: **port-as-is** = the verb's control flow and arithmetic are unchanged and only the
time representation or a shared helper moves; **re-derive** = the algorithm's structure changes
(rewindow over rationals, split source map, ripple exactness, speed mean, engine coupling);
**drop** = not carried as a recorded command.

---

## 1. Command model

### 1.1 Shape: `std::variant`, not a class hierarchy

The reference editor's `Command` is a serde-tagged enum — one value type with a tag and per-variant fields
(the reference project crate's `src/commands/mod.rs:224-231`). The C++ mirror is a closed `std::variant` of plain
per-verb structs, all in namespace `genesis::project`:

```
using Command = std::variant<
    AddMedia, RemoveMedia, SetMediaPlaceholder, FillSlot, Batch,
    AddClip, AddClipAtFirstFree, AddTextClip, AddLayerClip,
    SetClipSpeedCurve, SetClipKey, ClearClipKey, ClearClipKeys,
    SetEffectKey, ClearEffectKey, ClearEffectKeys,
    SetClipCutout, AddCutoutStroke,
    MoveClips, TrimClip, SplitClips, ReplaceClipMedia, FreezeFrame,
    MergeClips, RemoveClips, UpdateClip, SetClipSpeed, SetClipTransform,
    DetachAudio, ReattachAudio,
    AddTrack, RemoveTrack, SetTrackFlag,
    AddTimeline, RemoveTimeline, SetTimelineVideo, RenameTimeline,
    SelectTimeline, MoveTimeline,
    AddFont, RemoveFont, UpdateMediaPath, SetMediaColorRange>;
```

Rationale, in order of weight:

1. **Value semantics for history.** A `HistoryEntry` stores a `Command` by value; a variant of
   structs is trivially copyable when its members are. A virtual base forces heap/`unique_ptr`
   ownership and indirection into every history record for no benefit.
2. **Exhaustive routing.** `std::visit` reproduces the reference editor's `match` (mod.rs:989-1054) and fails to
   compile when a variant is added without a handler — the same property the Rust match gives.
3. **Serde parity.** The tagged-enum shape maps onto the `{"op": ..., ...}` wire form the API layer
   needs (`docs/history/rewrite.md` §2 the reference `api` crate row), without committing to a JSON library now.
4. **No open set.** Commands are not a third-party extension point; a closed variant is correct.

Supporting value structs mirror the reference editor: `NewMedia` (mod.rs:179-218), `ClipPatch` with the three-way
`absent | null | value` semantics (mod.rs:85-153), `ClipMove` (mod.rs:69-78), `TrimEdge`
(mod.rs:48-54), `TrackFlag` (mod.rs:57-64), `Outcome` (mod.rs:694-706), `CommandError`
(mod.rs:713-746).

### 1.2 Application: `std::expected`

```
std::expected<Outcome, CommandError>
apply(const Command&, Project&, IdMinter&, UndoRecord&);
```

- `std::expected` is C++23 (R11) and matches the reference editor's `Result<Outcome, CommandError>` directly.
- `Outcome { std::optional<std::string> created_id; bool applied; }`. `applied == false` is the
  tolerated no-op that must record no history (mod.rs:694-706, editor.rs:143-145).
- `CommandError` is a small enum carrying the user-facing sentence the reference editor treats as contract
  (mod.rs:708-746). Decision Q8: whether those sentences stay in the model layer or move to the UI
  layer; default is to keep them next to the error that produces them, as the reference editor does.
- `IdMinter` (mod.rs:752-795) is owned by the `Editor` and passed by reference; `adopt_project`
  (mod.rs:781-794) advances it past every id a restored file uses.

The `apply` free function is the dispatcher; each verb group lives in its own translation unit
(§6). Non-finite guards (`Command::has_non_finite`, mod.rs:843-961) become a free
`has_non_finite(const Command&)` checked once at the top of `apply`, before dispatch — JSON cannot
spell NaN but a caller can, and one stored poisons every duration it touches.

### 1.3 Composition: `Batch`, atomic without a whole-project clone

`Batch` is a `Command` variant holding `std::vector<Command>`; nesting is legal (mod.rs:266-269).
The reference editor applies a batch to a staged **clone** of the whole project and commits only on full success
(mod.rs:990-1009). The host does not clone the project: it applies members in order while
accumulating the pending `UndoRecord`, and on a member failure reverts the accumulated record and
returns the error. The observable contract is identical (all-or-nothing, one undo step); the cost
is the touched delta instead of the project size.

### 1.4 Ownership and boundaries

- `Editor` owns `Project`, `IdMinter`, and `History` **by value** (mirrors the reference editor's `Editor`,
  editor.rs:45-52). No `shared_ptr`, no `Arc`. The reference editor needed `Arc<Clip>`/`Arc<Timeline>`
  (the reference project crate's `src/model.rs:1662,1821`) only to make snapshot undo cheap; §2 removes that
  requirement, so the host model stays plain values, as it already is (`src/project/ModelTypes.h`).
- The UI and the future API layer never see a mutable `Project`; all mutation goes through
  `Editor::apply`, so nothing changes without a recorded inverse (editor.rs:109-113).
- No engine type crosses this layer (R6). Commands are pure functions over the project model.

---

## 2. Undo / redo: a command stack with scoped before-images

The reference editor's undo is whole-state snapshots with depth 200 (`editor.rs:32-52`), cheap because of `Arc`
sharing. Architecture §7 requires commands over the domain model, and rewrite-plan §3(c) sets the
acceptance bar: **undo/redo restores graph state exactly**, including the coupled HDR step, with
gesture coalescing and view-state exclusion. Without `Arc`, whole-state snapshots per edit are
O(project) each and defeat the intent, so the host records a **scoped delta**.

### 2.1 Stack structure

```
struct HistoryEntry {
    Command command;              // forward edit, replayed on redo
    UndoRecord before;            // the inverse: before-images of what it wrote
    std::optional<std::string> gesture;
};

class History {
    std::deque<HistoryEntry> undo_;   // oldest at the front
    std::deque<HistoryEntry> redo_;
};
```

- Depth 200 is a **product decision to restate**, not inherit silently (rewrite-plan §3(c),
  §7 Q2). The default in the plan is 200 for behavioral continuity.
- Eviction is from the front of `undo_` (editor.rs:157-159).

### 2.2 How the inverse is represented: before-images, not mirrored commands

`UndoRecord` is an ordered list of entity before-images plus the two counters that are not in the
model:

- `id_counter` — the `IdMinter` counter before the edit.
- `active_timeline_id` — before the edit.
- `std::vector<ClipSnapshot>` / `TrackSnapshot` / `TimelineSnapshot` / `MediaSnapshot` /
  `FontSnapshot`, each `{ std::string id; std::optional<T> before; }`. `nullopt` means *absent
  before* (undo removes it); a value means *present before* (undo replaces or re-inserts it).

Undo walks the record in reverse and restores each entity; redo re-applies the stored `Command`.
Because `apply` is deterministic over `(Project, IdMinter)` and the counter is restored, re-running
the forward command reproduces the after-state exactly — so no after-image is stored.

**Why before-images and not a mirrored inverse command.** Three operations are derived or lossy and
cannot be inverted by re-running a symmetric algorithm:

- `Clip::rewindow_keys` re-anchors every key through split/trim/merge (`ModelTypes.h:1367`,
  `absorb_keys` at `1452`); the inverse is not the same call with swapped arguments.
- `tidy_touched` runs `Clip::tidy` over every clip the command replaced (`editor.rs:170-197`); the
  clamps (`ModelTypes.h:1096-1204`) are not a bijection.
- `follow_first_hdr` promotes a timeline to HLG inside the step that added its first HDR clip
  (`editor.rs:205-247`); its inverse is not a command, it is "the colour it had".

Storing the before-image guarantees exact restore by construction. This is the per-edit
snapshot-equivalence behind the same interface that rewrite-plan §3(c) explicitly permits; the
plan chooses it for the derived cases rather than discovering divergence later.

**Cost.** Only entities the command wrote are copied. A multi-clip move copies the moved clips; a
timeline flag flip copies one timeline header. This matches the reference editor's "copies only the clip it
writes" property (editor.rs:5-10) without `Arc`.

### 2.3 Application and history in one call

```
Editor::apply(command)                 -> apply_within(std::nullopt, command)
Editor::apply_within(gesture, command) -> std::expected<Outcome, CommandError>
```

Sequence (mirrors editor.rs:133-163):

1. If `is_view_state(command)` — `SelectTimeline` / `MoveTimeline` (mod.rs:832-837) — apply without
   touching history, and return. Q5 asks whether these stay in the variant at all.
2. `apply` into the project while filling an `UndoRecord`.
3. On error, discard the record; history is untouched (editor.rs:121-123).
4. If `!outcome.applied`, discard the record; nothing is recorded (editor.rs:143-145).
5. Run the derived steps (`tidy_touched`, `follow_first_hdr`) as part of the same step and extend
   the record with anything they write, so undo takes them back with the edit.
6. Coalesce: if `gesture` is non-null and equals the last entry's gesture and no other step
   intervened, **discard the new record and keep the earlier one**, so undo lands where the gesture
   began (editor.rs:148-160). Otherwise push a new entry.
7. Clear `redo_`.

`end_gesture()` clears the last entry's gesture (editor.rs:253-257). The reference editor has no time-based
gesture timeout — the caller decides when a drag ends; Q2 asks whether the host adds one.

### 2.4 Dirty tracking

`Editor::dirty()` is true when a change has occurred since the last save. Track it as a saved
counter/token: `History` bumps a monotonically increasing `generation_` on every recorded step and
on every view-state edit (view state is saved but not undoable, editor.rs:829-837); the editor
stores the generation at save time. `dirty = generation_ != saved_generation_`. This distinguishes
"undo back to the saved point" from "never saved", which a bare `can_undo` cannot.

---

## 3. Rational time at edit points

### 3.1 Where the boundary is

`docs/architecture.md` §7: rational time everywhere, no float seconds for edit points. The host
already has `genesis::core::Rational`, `FrameRate`, `TimeRange` (`src/core/ClipTimeline.h:30,107,151`)
and `Rational::approximate(double)` is documented as "the seam where a UI's double becomes exact"
(`ClipTimeline.h:46-50`).

**Time-valued fields in the project model become `Rational`.** These are seconds:

| Type | Field | Host line |
|---|---|---|
| `Clip` | `start` | `ModelTypes.h:993` |
| `Clip` | `duration` | `ModelTypes.h:996` |
| `Clip` | `source_start` | `ModelTypes.h:998` |
| `Clip` | `fade_in`, `fade_out` | `ModelTypes.h:1001-1003` |
| `Transition` | `duration` | `ModelTypes.h:844` |
| `Stroke` | `at` (source instant) | `ModelTypes.h:689` |
| `MediaItem` | `duration` (probe metadata used to seed clip length) | `ModelTypes.h:308` |

**Fields that stay `double`** because they are dimensionless, not seconds: the key/param fractions
`ClipKey::at`, `ParamKey::at`, `SpeedPoint::at` (fractions of clip length); `SpeedPoint::speed`;
every transform, blend, ease, colour and opacity scalar; `VideoSettings.rate_num/rate_den` is already
exact (`ModelTypes.h:1515-1517`). Rationalising the fractions is optional future work, not this
slice; it changes no semantics and widens the diff across `rewindow_keys`/`absorb_keys`.

`MediaItem::duration` is the one judgement call: it is probe metadata, but it seeds a clip's
default duration (mod.rs:52-55, clips.rs:610-623). Recommend `std::optional<Rational>`, since the
value crosses into a clip; if the probe reports a decimal double the command boundary converts once
with `Rational::approximate`. Q4 confirms.

### 3.2 The seam

The command/UI/API boundary accepts `double` seconds where a caller has one; the command layer
converts exactly once (`Rational::approximate`, or `Rational::parse` for a `"num/den"` string like
`MediaItem::frame_rate_fraction`, `ModelTypes.h:319`). No `double` is ever stored at an edit point,
and no `Rational` round-trips through `double` (`ClipTimeline.h:68-71`).

### 3.3 Arithmetic that must be re-derived over rationals

- `Clip::blank` minimum duration `1.0/60.0` (`ModelTypes.h:1079`) and `Clip::tidy`'s floor
  (`ModelTypes.h:1097`) become `Rational{1, 60}`.
- `split_source` (`clips.rs:706-708`): `source_start + offset * speed` — exact rational arithmetic.
- `rewindow_keys` / `absorb_keys` (`ModelTypes.h:1367,1452`): the stored keys are still fractions
  (`double`), but `old`/`from`/`to` are `Rational`; the fraction conversion is the single documented
  conversion point, and `KEY_EPSILON` (`ModelTypes.h:268`) stays a fraction tolerance.
- `Timeline::duration()` (`ModelTypes.h:1632-1638`) and `Timeline::duration` in the core type
  (`ClipTimeline.h:499`) become exact.
- Trim (`clips.rs:178-249`), `close_gaps` (`clips.rs:578-608`), ripple room (`clips.rs:630-655`) and
  `JOIN_EPSILON` (`clips.rs:568`) comparisons become exact integer comparisons on rationals (or a
  rational epsilon). This is the "keys stay on their instant of the picture" correctness hotspot
  (rewrite-plan §3(d)).
- `SetClipSpeed` / `SetClipSpeedCurve` duration = `source_covered / speed`
  (`properties.rs:90-111,270-292`): rational division; `SpeedCurve::mean` returns a `double`, so the
  mean becomes the one place a rational is approximated — Q4 asks whether `SpeedCurve::mean` should
  gain a rational return.

### 3.4 Change now or later

**Recommendation: change the model's time fields now.**

- The serializer does not exist. `doc.rs` is unported, `DOCUMENT_VERSION`/`MIGRATIONS`
  (the reference project crate's `src/doc.rs:41,62`) have no host counterpart, and the model's `extra` maps are
  explicitly not ported (`ModelTypes.h:352-353,397-398,1064-1065,1576-1577,1680-1681`). There is no
  document format to migrate and no external caller of the command API.
- Cost now: seven field type changes, the functions in §3.3, and the command arms that read them.
- Cost later: a JSON migration under `docs/history/rewrite.md` §7 Q3, a compatibility proof for every
  file written before the change, plus re-touching the same functions. Strictly larger.
- Risk of waiting: every test written against `f64` seconds must be rewritten, and float equality
  bugs can be baked into fixtures.

---

## 4. Command inventory

The reference editor's `Command` enum has **43 variants** (the reference project crate's `src/commands/mod.rs:231-688`), split
across `clips.rs`, `properties.rs`, `media.rs`, `timelines.rs`, `audio.rs`, `tracks.rs`, plus the
`Batch` composition in `mod.rs`.

**Tally: port-as-is 26, re-derive 15, drop 2** (view-state demoted). `speed.rs` adds two shared
helpers, both port-as-is.

### 4.1 Port-as-is (26)

Control flow and arithmetic unchanged; only the time type (§3) or a shared helper moves.

| Verb | Reference | Notes |
|---|---|---|
| `AddMedia` | media.rs:18-51 | path-dedup; mint `m` |
| `RemoveMedia` | media.rs:132-147 | sweeps clips on all timelines |
| `SetMediaPlaceholder` | media.rs:53-66 | flag toggle |
| `FillSlot` | media.rs:68-130 | in-place swap, all timelines |
| `AddFont` | media.rs:149-158 | path-dedup |
| `RemoveFont` | media.rs:160-170 | titles keep the family |
| `UpdateMediaPath` | media.rs:172-182 | relink |
| `SetMediaColorRange` | media.rs:184-194 | model field; engine read is adapter (§4.4) |
| `AddTrack` | tracks.rs:18-31 | |
| `RemoveTrack` | tracks.rs:33-48 | floor of one |
| `SetTrackFlag` | tracks.rs:50-68 | |
| `AddTimeline` | timelines.rs:18-52 | four fresh lanes |
| `RemoveTimeline` | timelines.rs:69-93 | floor of one |
| `SetTimelineVideo` | timelines.rs:54-67 | `is_sane` refusal, no clamp |
| `RenameTimeline` | timelines.rs:95-111 | trim, refuse blank |
| `UpdateClip` | properties.rs:18-88 | the `ClipPatch` arms |
| `SetClipTransform` | properties.rs:294-330 | scalar clamps |
| `SetClipCutout` | properties.rs:113-123 | `Cutout::tidy` |
| `AddCutoutStroke` | properties.rs:125-140 | `Stroke::tidy`; `Stroke::at` becomes Rational |
| `SetClipKey` | properties.rs:142-175 | per-property clamps |
| `ClearClipKey` | properties.rs:177-191 | |
| `ClearClipKeys` | properties.rs:193-203 | |
| `SetEffectKey` | properties.rs:205-229 | |
| `ClearEffectKey` | properties.rs:231-249 | |
| `ClearEffectKeys` | properties.rs:251-268 | |
| `Batch` | mod.rs:266-269, 990-1009 | composition re-derived for atomicity (§1.3); verb unchanged |

### 4.2 Re-derive (15)

The verb survives; its arithmetic or its engine coupling changes.

| Verb | Reference | Why re-derived |
|---|---|---|
| `AddClip` | clips.rs:18-45 | rational start/duration; ripple room (clips.rs:630) exact |
| `AddClipAtFirstFree` | clips.rs:47-67 | rational placement; free-lane predicate exact |
| `AddTextClip` | clips.rs:69-120 | rational duration/offset; Qt text is a later `replace` |
| `AddLayerClip` | clips.rs:122-154 | rational duration; effect id from the Pack model |
| `MoveClips` | clips.rs:156-176 | rational `start` |
| `TrimClip` | clips.rs:178-249 | rational head/tail, in-point, rewindow, ripple; exact edge |
| `SplitClips` | clips.rs:251-306 | exact `split_source`; rewindow over rationals |
| `MergeClips` | clips.rs:495-530 | exact touch test (`JOIN_EPSILON`); rewindow + absorb |
| `FreezeFrame` | clips.rs:362-493 | split + ripple + still; still extraction is adapter work |
| `ReplaceClipMedia` | clips.rs:308-360 | probe/adapter coupling; rational in-point |
| `RemoveClips` | clips.rs:532-561 | exact `close_gaps` (clips.rs:578) |
| `SetClipSpeed` | properties.rs:90-111 | rational duration from held source coverage |
| `SetClipSpeedCurve` | properties.rs:270-292 | curve mean → rational duration (Q4) |
| `DetachAudio` | audio.rs:18-108 | GES sound semantics; multi-track naming |
| `ReattachAudio` | audio.rs:110-143 | GES sound semantics |

### 4.3 Drop (2)

| Verb | Reference | Verdict |
|---|---|---|
| `SelectTimeline` | timelines.rs:113-125 | **drop from the recorded stack**; view state, saved not undoable (mod.rs:832-837, editor.rs:138-140) |
| `MoveTimeline` | timelines.rs:127-145 | **drop from the recorded stack**; view state |

Q5 asks whether these leave the `Command` variant entirely or stay as non-undoable members.

### 4.4 `speed.rs`

`speed.rs` is two shared helpers, not commands: `curve_of` (speed.rs:14-20) and `mean_of`
(speed.rs:24-26). Both are **port-as-is**: `curve_of` is already `SpeedCurve::create`
(`src/core/SpeedCurve.h:40`), and `mean_of` is `SpeedCurve::mean` (`SpeedCurve.h:56`). The command
layer calls the core helper; no new speed code exists in this layer. Q4 covers whether `mean`
returns a rational.

### 4.5 Engine-coupled verbs and the host boundary

`FreezeFrame` (still jpg), `ReplaceClipMedia` (enhanced/reversed copy), `DetachAudio`/`ReattachAudio`
(sound extraction), and `SetMediaColorRange` (reading levels) depend on probe/extract work that is
Phase 2/3 adapter and engine work (R5). The **command and model shapes** port here; the media work
does not. A command that needs a probe artifact takes the already-probed `NewMedia` as an argument
(`FreezeFrame.still`, mod.rs:516-518), so the layer stays engine-free.

---

## 5. Serialization seam

The reference project crate's `src/doc.rs` is the document layer: migrations (`doc.rs:62-71`), `DOCUMENT_VERSION`
(`doc.rs:41`), tolerant parse into `extra` maps (`doc.rs:170-174`), settle (`doc.rs:178-216`) and
write with the flat-mirror fields (`doc.rs:314-361`). It is **not** ported by this plan and **not
designed here**.

The seam is marked, not opened:

- The model already reserves the `extra` maps this layer owns (`ModelTypes.h:352-353,397-398,
  1064-1065,1576-1577,1680-1681`). No other file may use them.
- The planned host file is `src/project/Document.h` / `Document.cpp`, created **only when
  `docs/history/rewrite.md` §7 Q3 is answered**. If Q3 is "read `concat.json` one-way", the layer adds
  the reference editor's migration table; if Q3 is "new format", the command/`extra` wire format is free.
- Nothing in the command or undo layer serializes. `Command` being a tagged variant (§1.1) keeps
  the option open without paying for it now.

---

## 6. File layout

All under `src/project/`, namespace `genesis::project`, standard C++23, no Qt, no GStreamer.

| File | Contents | Depends on |
|---|---|---|
| `ModelTypes.h` (exists) | project model; time fields become `Rational` (§3) | `core/Animation.h`, + `core/ClipTimeline.h` for `Rational` |
| `IdMinter.h` / `.cpp` | counter, `next`, `adopt`, `adopt_project` (mod.rs:752-795) | none |
| `CommandTypes.h` | `Command` variant, `NewMedia`, `ClipPatch`, `ClipMove`, `TrimEdge`, `TrackFlag`, `Outcome`, `CommandError`, `is_view_state`, `has_non_finite` | `ModelTypes.h`, `IdMinter.h` |
| `commands/Apply.h` / `Apply.cpp` | `apply` dispatcher; non-finite guard; `Batch` atomicity | `CommandTypes.h`, verb groups |
| `commands/ClipPlacement.cpp` and `commands/ClipReshape.cpp` | placing/moving/cutting verbs | `commands/Apply.h` |
| `commands/Properties.cpp` | patch/speed/transform/keys/cutout verbs | `commands/Apply.h` |
| `commands/Media.cpp` | bin, slots, fonts, relink, colour range | `commands/Apply.h` |
| `commands/Timelines.cpp` | timeline verbs | `commands/Apply.h` |
| `commands/Audio.cpp` | detach/reattach | `commands/Apply.h` |
| `commands/Tracks.cpp` | lane verbs | `commands/Apply.h` |
| `Undo.h` / `Undo.cpp` | `UndoRecord`, `*Snapshot`, `History`, `HistoryEntry`, coalescing | `ModelTypes.h`, `CommandTypes.h` |
| `Editor.h` / `.cpp` | owns `Project`, `IdMinter`, `History`; `apply`, `apply_within`, `undo`, `redo`, `end_gesture`, `tidy_touched`, `follow_first_hdr`, dirty token | all of the above |
| `Document.h` / `.cpp` | serialization — **seam only**, §5, gated on Q3 | `ModelTypes.h` |

Dependency order: `IdMinter` → `CommandTypes` → `commands/*` → `Undo` → `Editor`; `Document` last and
optional. `SpeedCurve` comes from `core/SpeedCurve.h`, not a new file. No catch-all `utils`/`helpers`
file; every file above has a single named responsibility.

---

## 7. Verification strategy

The whole layer is engine-independent: it links `src/core/*.cpp` and `src/project/*.cpp` only. No
GES, no Qt, no media. This is the Phase 2 acceptance requirement ("adapter contract is interface
only; no GES type appears", rewrite-plan §4 Phase 1) made testable before any adapter exists.

Planned tests under `tests/project/`, using the existing no-framework `check`/`check_near` style
(`tests/CoreTests.cpp:19-37`):

1. **Per-verb behavior** — mirror `lib.rs`'s test list (`lib.rs:35-3791`): clamps, tolerated
   no-ops with `applied == false` (lib.rs:2878, 2901), batch atomicity (lib.rs:2506, 2532), ripple
   delete (lib.rs:292), magnetic trim (lib.rs:485), split/merge continuity (lib.rs:998), freeze
   (lib.rs:876), detach/reattach (lib.rs:1851, 1880), timeline floors (lib.rs:1995), fonts
   (lib.rs:3383), NaN refusal (lib.rs:1106).
2. **Undo exactness** — the acceptance bar: for every verb and a random command sequence,
   `undo(apply(cmd)) == before` and `redo(undo) == after`. Compare whole `Project` with
   `operator==` (`ModelTypes.h:1752`).
3. **Coupled and derived steps** — undo of a trim/split/merge restores keys on their exact
   instants (lib.rs:1549); a first HDR clip and its timeline promotion undo together (lib.rs:144);
   `tidy` round-trips (lib.rs:1355).
4. **Gesture coalescing and view-state exclusion** — a named gesture is one step (lib.rs:1230);
   switching/reordering tabs records nothing (lib.rs:1275); depth-200 eviction (lib.rs:2977).
5. **Rational time** — edit points round-trip without a `double`; frame-grid equality at 30000/1001;
   `1/60` floors exact; `rewindow_keys` keeps the instant invariant.
6. **Id minting** — restored ids are never re-issued (lib.rs:2236); undo/redo keep ids stable.

Build check: `g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -I src src/core/*.cpp
src/project/*.cpp tests/project/*.cpp`. One parse plus one test run is the evidence per phase; the
model layer is already proven to compile under the same flags.

---

## 8. Open questions

Each blocks a named decision.

1. **Fidelity target** (rewrite-plan §7 Q2). Exact the reference editor's undo semantics — gesture granularity,
   `Clip::tidy` clamps, HDR coupling — or Genesis-0 rules? Blocks the §7 undo acceptance tests.
2. **Depth and gesture timeout** (rewrite-plan §3(c)). Keep depth 200 and caller-driven gestures,
   or set a host depth/timeout? Blocks `History` constants (§2.1) and `end_gesture` (§2.3).
3. **Project-file compatibility** (rewrite-plan §7 Q3). Read/migrate `concat.json`, or a new format?
   Blocks the §5 seam and whether the command wire format must match the reference editor's.
4. **Rational migration timing.** Confirm changing `ModelTypes.h` time fields now, with no
   serializer, rather than keeping `f64` until Q3. Also: does `MediaItem::duration` and
   `SpeedCurve::mean` become rational? Blocks §3 and the field diff.
5. **View-state commands.** Do `SelectTimeline`/`MoveTimeline` stay in the `Command` variant as
   non-undoable members, or move to a separate view-state type? Blocks `CommandTypes.h` shape.
6. **Inverse representation.** Accept scoped before-image `UndoRecord`s, or require true mirrored
   inverse commands for structural edits? Blocks §2's acceptance and the `Undo.h` API.
7. **Template slots.** Are `SetMediaPlaceholder`/`FillSlot` in scope now (product templates) or
   deferred? Blocks their inventory verdict (§4.1).
8. **Deferred features.** Text (Qt rasteriser is a later `replace`) and cutout/vision (the
   reference `vision` crate is deferred in rewrite-plan §2). Ship the inert model commands now, or defer them with their
   features? Blocks the inventory's completeness.

---

## Appendix — citation index

The reference editor (read-only checkout):

- the reference project crate's `src/commands/mod.rs` — `Command` enum 231-688; `ClipMove` 69-78; `ClipPatch`
  85-153; `NewMedia` 179-218; `Outcome` 694-706; `CommandError` 713-746; `IdMinter` 752-795;
  `next_numbered` 815-826; `first_line` 798-810; `is_view_state` 832-837; `has_non_finite` 843-961;
  `assign` 968-975; `apply` 981-1054.
- the reference project crate's `src/commands/clips.rs` — 18, 47, 69, 122, 156, 178, 251, 308, 362, 495, 532, 568,
  578, 610, 630, 659, 680, 706, 712.
- the reference project crate's `src/commands/properties.rs` — 18, 90, 113, 125, 142, 177, 193, 205, 231, 251, 270,
  294.
- the reference project crate's `src/commands/media.rs` — 18, 53, 68, 132, 149, 160, 172, 184.
- the reference project crate's `src/commands/timelines.rs` — 18, 54, 69, 95, 113, 127.
- the reference project crate's `src/commands/audio.rs` — 18, 110.
- the reference project crate's `src/commands/tracks.rs` — 18, 33, 50.
- the reference project crate's `src/editor.rs` — 32, 35-52, 109-123, 133-163, 170-197, 205-247, 253-284.
- the reference project crate's `src/doc.rs` — 41, 62, 170, 178, 314.
- the reference project crate's `src/speed.rs` — 14, 24.
- the reference project crate's `src/model.rs` — 1662, 1821.
- the reference project crate's `src/lib.rs` — re-exports 24-33; tests 35 onward.

Host:

- `src/project/ModelTypes.h` — time fields 308, 689, 844, 993-1003; `Clip::blank` 1077;
  `Clip::tidy` 1096-1204; `reindow_keys` 1367; `absorb_keys` 1452; `Timeline` 1560; `Project` 1669.
- `src/core/ClipTimeline.h` — `Rational` 30, `approximate` 50, `FrameRate` 107, `TimeRange` 151.
- `src/core/SpeedCurve.h` — `create` 40, `mean` 56.
- `src/core/Animation.h` — `Ease`, `Key`, `KeyTrack`.
