# Genesis-0 — Compositor Path Decision

Status: Accepted (2026-09-29).

Date: 2026-09-29. Deciders: project owner + orchestrator.

Scope: the **desktop render/present path** — how GES-composited frames reach the Qt Quick scene
graph, and whether Genesis-0 keeps GES's compositor or builds a custom one. It is **not** the engine
choice (fixed by T1) or the UI toolkit (fixed by R1).

---

## 1. Context

`docs/history/rewrite.md` §3(a) named the highest risk in the whole plan: the reference editor's
renderer is a
first-class custom pipeline — `plan_frame(t)` produces a `FramePlan` that a single `WgpuCompositor`
draws with a working space of linear light, extended scRGB, and half floats — while GES composites
through GStreamer's `compositor`/`videoaggregator` and `gnlcomposition`, with no programmable
`FramePlan` equivalent. The fork was: keep GES's compositor and re-author effects against GStreamer
primitives, or build a custom RHI compositor fed by decoded frames (which contradicts "GES is the
engine" and duplicates GES work). `docs/history/rewrite.md` §7 Q5 and `docs/decisions/engine-choice.md`
§2 listed it as unresolved, and §3(a) made it the **Phase 0 gate**.

The reason a custom compositor was contemplated at all was the assumed absence of a zero-copy path
from GES output into the Qt Quick scene graph. `docs/decisions/engine-choice.md` §2 recorded the GES
desktop render-into-Qt-Quick spike as unrun; the gate could not be closed until the presentation half
was proven.

## 2. Spike evidence (verified 2026-09-29)

Spike at `spikes/ges-qt-render/` (`GesVideoItem.{h,cpp}`, `main.cpp`, `Main.qml`, `CMakeLists.txt`).
Sample media `/tmp/opencode/ges-spike/a.webm`. The engine-half run was recorded in
`docs/dependencies.md` when it passed (2026-09-28); its scratch artifacts under `/tmp` have since been
cleaned. Proven end to end, on a real Wayland display:

- **RHI backend and context wrap.** `QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL)` runs
  before window creation (`main.cpp:32`). Qt's RHI GL context is then wrapped for GStreamer via
  `QNativeInterface::QEGLContext` + `gst_gl_context_new_wrapped` (`GesVideoItem.cpp:88-147`),
  installed through a `GstBus` sync handler answering `GST_MESSAGE_NEED_CONTEXT` for
  `gst.gl.GLDisplay` and `gst.gl.app_context` (`GesVideoItem.cpp:259-329`).
- **Pipeline and caps.** The pipeline reaches PLAYING and produces
  `video/x-raw(memory:GLMemory), format=(string)RGBA, 640x360, texture-target=(string)2D` —
  GPU-resident, a single texture, zero CPU readback, inside Qt's own GL context (desktop GL 4.6).
- **Scene-graph wrap without `createTextureFromNativeObject`.** That API was removed in Qt 6.11. The
  frame is wrapped instead as `rhi->newTexture(QRhiTexture::RGBA8, size)` →
  `createFrom({textureId, 0})` → `window->createTextureFromRhiTexture(rhiTex)` →
  `QSGSimpleTextureNode::setTexture(...)` in `updatePaintNode` (`GesVideoItem.cpp:429-513`).
- **Threading.** All GL work happens in `updatePaintNode` (render thread); the streaming thread only
  reads the texture id and refs the `GstBuffer` (`GesVideoItem.cpp:384-427`). Frames commit as
  textured nodes (`committed textured node #1..#3`, distinct GL ids); the run reaches EOS, exit 0, no
  GL warnings under `GST_DEBUG="GST_GL*:4"`. A programmatic screenshot verified correct SMPTE
  colour-bar colours and orientation (transform: `NoTransform`).
- **Engine half** (already recorded in `docs/dependencies.md` §"Phase 0 spike result"): GES builds a
  timeline from a host model via `ges_layer_add_asset`, renders end-to-end, frames stay GPU-resident
  as `GLMemory` with `texture-target=2D`; chained `glshader` passes compose.
- **Build requirement.** `Qt6::GuiPrivate` is required for `<rhi/qrhi.h>` on this distro
  (`CMakeLists.txt:12-15,35`).
- **Residual risk, unresolved.** Sharing Qt's **exact** EGL context between GStreamer's streaming
  thread and Qt's render thread is a latent `eglMakeCurrent` hazard. It did not crash, but a
  **share-group (separate) context** is the robust production arrangement.

## 3. Decision

**Keep GES's compositor.**

The zero-copy GL handoff of GES output into the Qt Quick scene graph is proven, which removes the main
reason a custom Qt RHI compositor was contemplated. Genesis-0 presents GES-composited frames through
the scene graph with no CPU readback, and effects are re-authored against GStreamer primitives.

The **`FramePlan` adapter boundary is therefore not required** for the render path. The reference
editor's
`FramePlan` value was backend independence; with GES as the only compositor there is no second
compositor to hold parity against. The host model → GES graph compilation remains required, as
Phase 2 (`docs/history/rewrite.md` §4 Phase 2), independent of this decision.

## 4. Alternatives considered

### 4.1 Custom Qt RHI compositor (not selected)

Build a Qt RHI compositor fed by decoded frames, preserving the `FramePlan` contract and the
reference editor's linear-light working space. This is the branch §3(a) warned would contradict
"GES is the engine" and
duplicate GES work. It is not selected because the spike proves the presentation path GES's
compositor needs, so the custom compositor no longer buys a proven-missing capability. It is not
foreclosed permanently; see §6.

### 4.2 Port the reference editor's wgpu compositor (rejected)

Already rejected in `docs/decisions/engine-choice.md` §8: the reference editor is a Rust engine with
a GPU
compositor, while Genesis-0 is C++23 + Qt6 Quick/QML + GStreamer/GES, so incorporation is a port,
not a drop-in. This spike does not reopen it.

## 5. Consequences

**What this forecloses (for now).** A custom RHI compositor as the default render path; and the
expectation that `FramePlan` survives as the render-path contract. The compositor is GES's, and effect
execution is re-authored against GStreamer primitives under the effect-runtime decision
(`docs/history/rewrite.md` §4 Phase 4).

**What stays required.** The host model → GES graph compilation (Phase 2) is unaffected: the adapter
still compiles the host graph into a GES pipeline without leaking engine types (R5/R6).

**Follow-up.** Replace the exact-EGL-context sharing with a **share-group (separate) GL context**
between GStreamer's streaming thread and Qt's render thread. The spike used Qt's exact context and did
not crash, but the `eglMakeCurrent` hazard is latent and the separate-context arrangement is the
robust production shape.

**Residual risk carried forward.** The `eglMakeCurrent` hazard above, and the render-layer items §3(a)
did not settle (named-intermediate multi-pass graphs, extended-linear half-float working space,
non-blocking scopes compute pass). Those items are separate from this decision: they bear on the
effect execution path (Phase 4), not on whether GES presents into Qt Quick.

## 6. Revisit triggers

Adopt a custom RHI compositor only if a future need demands it:

1. **Multi-stream mixing inside Qt** that GES's compositor cannot express.
2. **Per-layer effects** the GES compositor cannot express.
3. **The `FramePlan` contract proving necessary** as an adapter boundary.

## 7. Evidence index

| Fact | Location |
|---|---|
| RHI backend forced to OpenGL before window creation | `spikes/ges-qt-render/main.cpp:32` |
| Qt EGL context wrapped for GStreamer | `spikes/ges-qt-render/GesVideoItem.cpp:88-147` |
| `GST_MESSAGE_NEED_CONTEXT` sync handler (`gst.gl.GLDisplay`, `gst.gl.app_context`) | `spikes/ges-qt-render/GesVideoItem.cpp:259-329` |
| GL memory classification; GPU-resident frame | `spikes/ges-qt-render/GesVideoItem.cpp:340-382` |
| Scene-graph wrap without `createTextureFromNativeObject` | `spikes/ges-qt-render/GesVideoItem.cpp:429-513` |
| Streaming thread reads id and refs buffer only | `spikes/ges-qt-render/GesVideoItem.cpp:384-427` |
| `Qt6::GuiPrivate` for `<rhi/qrhi.h>` | `spikes/ges-qt-render/CMakeLists.txt:12-15,35` |
| Sample media path | `spikes/ges-qt-render/main.cpp:41` |
| Engine half: `ges_layer_add_asset`, end-to-end render, `GLMemory`/`texture-target=2D`, chained `glshader` | `docs/dependencies.md` §"Phase 0 spike result" |
| Highest-risk fork recorded (custom compositor vs GES) | `docs/history/rewrite.md:98-137` (§3(a)); §7 Q5 |
| Engine-choice unresolved gate | `docs/decisions/engine-choice.md` §2 |
| The reference editor wgpu compositor rejection | `docs/decisions/engine-choice.md` §8 |

See `docs/history/rewrite.md` §3(a)/§4/§7 Q5 for the annotated gate, `docs/dependencies.md` for the spike
toolchain and caps, and `docs/decisions/engine-choice.md` for the engine decision this record is
constrained by.

## Revisit considered and declined (2026-09-30)

The revisit trigger fired in practice: working on effect passes and per-word title reveal produced a
run of compositor-level obstacles, which is exactly the condition this record said would reopen the
question. **It was considered and declined — GES's compositor stays.** The project owner's call:
use the existing engine, build capability rather than a renderer.

What the obstacles turned out to be, and why they do not justify a custom compositor:

- **A second texture is the whole problem, and it is solved by one element.** Stock `glshader`
  binds exactly one texture and a second `sampler2D` silently aliases unit 0
  (`docs/decisions/effect-execution.md` §6). `src/adapters/engine/ges/effects/TextureMix.{h,cpp}` — a
  `GstGLMixer` subclass binding N textures into one shader, pixel-verified (RED+GREEN -> YELLOW) —
  covers the entire multi-texture class: the 5 named-intermediate packs, per-word reveal, and any
  future pack needing a LUT, mask or intermediate. **One element per capability class, not per
  effect.**
- **The remaining obstacles are capability-shaped, not architecture-shaped**: caps templates,
  `NEED_CONTEXT` handling, stage ownership, explicit `glUniform1i`. Each is paid once per element
  type. That is a tax, not a wall.

Two things an element **cannot** buy, accepted as the cost of this decision:

1. **A half-float working space between layers.** GES's compositor negotiates RGBA8 between
   elements; an element may hold a float target internally but inter-layer blending stays 8-bit.
   Recorded as accepted, not open — `docs/decisions/colour-working-space.md` states the same conclusion
   from the format side.
2. **Inter-layer compositing, transitions and the frame plan** remain GES's.

Position B (GES for media I/O, our own compositor) and position C (a wgpu engine) stay recorded as
*declined*, with the reasoning above. Any future reopen needs evidence that a capability is
unreachable by an element — not merely awkward.
