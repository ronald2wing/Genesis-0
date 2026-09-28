# Genesis-0 — Host Core Language Decision

Status: **Accepted** (2026-09-28, project owner). Checked: 2026-09-28. No language benchmark,
performance, or parity claim is made or implied by this document. It records a language choice for
the host core only.

Date: 2026-09-28. Deciders: project owner + orchestrator.

Companion evidence: `docs/architecture.md` (R5/R6/R11, T1/T3/T4, §1, §7, §10),
`docs/rewrite-plan.md` (§1, §2, §5, appendix), `docs/decisions/engine-choice.md` (§6, §8),
`docs/decisions/licensing.md` (§0), `docs/dependencies.md`.

Scope of this decision: the **host core** — the portable project model, rational-time/timeline math,
command dispatch, and render coordination. It is **not** the engine (GStreamer/GES, fixed by T1) and
**not** the UI (Qt6 Quick/QML, fixed by R1). The question is narrower than "what language is
Genesis-0 written in".

---

## 1. Context

The host core is currently standard C++23. That choice is **inherited, not derived**. It entered
through the stack fixed for the Concat rewrite: `docs/rewrite-plan.md` §1 targets "C++23 host core +
Qt6 Quick/QML + GStreamer/GES" (`docs/rewrite-plan.md:11`), and `docs/decisions/engine-choice.md` §6
states the host "defines the project/domain/effect graph and rational time in standard C++23"
(`docs/decisions/engine-choice.md:87-88`). R11 then made it a settled requirement: host core, engine
adapter, and tooling use **standard C++23**, no closed compiler extensions
(`docs/architecture.md:44`).

No prior document argues *why* C++ rather than another language. The user asked whether C++ is
faster and why not Ruby. That question is fair: the existing records justify the engine, the UI
toolkit, and the license, but not the host language. This record either confirms C++ on its merits or
surfaces a real alternative. It is written as **Proposed** because the answer depends on information
held only by the user (§5).

The constraints that actually bind this choice, from the cited records:

- **Qt6 Quick/QML + RHI** (R1, T4): the host drives Qt Quick and the Qt RHI
  (`docs/architecture.md:34,52`). Qt is a C++ framework with a first-party C++ API.
- **GStreamer/GES** (T1): the host drives GES through a C API (`docs/decisions/engine-choice.md:15-18`).
- **Transliteration** (rewrite-plan §1/§2): Concat's Rust is being ported/re-derived; the source
  is **84,370 Rust lines / 132 files across 16 crates** (`docs/rewrite-plan.md:27-30,553-556`).
- **Rational time and a command model** live in the host (`docs/architecture.md:145-151,173-182`).
- **Three desktop OS families** (Windows, macOS, Linux). Mobile is out-of-repo — a separate mobile
  repository (future), not an engine or host concern here.
- **Extensions are the product differentiator** and have a native authoring surface (native
  extensions contributing Qt/QML panels; the web mini-app/WASM sandbox is **dropped**,
  `docs/architecture.md` §10).

## 2. Decision

**Confirm the host core in standard C++23.** R11 stands unchanged.

The reasoning is integration first, transliteration second, and portability third. Raw speed is not
part of the argument (see §3).

1. **Both host boundaries are C/C++ with first-party APIs.** Qt6 Quick/QML and the Qt RHI (T4) are
   C++; GES (T1) is a C API. A C++ host calls both directly, with no binding layer, no second
   runtime, and no FFI boundary on the render path. For every other candidate, one or both
   boundaries become a binding project.
2. **The logic core is same-family with the port source.** Of 16 Concat crates, **7 (37,591 LOC,
   ~45% of the Rust)** carry behavior worth porting/re-deriving — 1 `port` (`concat-core`) and 6
   `re-derive` (`concat-project`, `concat-effects`, `concat-export`, `concat-host`, `concat-api`,
   `concat-perf`) (`docs/rewrite-plan.md:69-86`). Rust→C++ maps directly: `enum`→`std::variant`,
   `Result<T,E>`→`std::expected`, ownership→RAII/`unique_ptr`, `Arc`→`shared_ptr`, traits→concepts.
   The plan records this as **re-derivation, not mechanical translation**
   (`docs/rewrite-plan.md:42-47`); the point is that the concept distance is small. Rust→Ruby/Python
   has no such mapping (§3).
3. **A compiled, runtime-free host bundles cleanly on the desktop OS families.** C++ links against
   the Qt/GStreamer libraries the app already ships. Rust is comparable (static, no runtime). A
   dynamic language adds an interpreter to every package.

**This is not a hybrid that moves the host language.** The real hybrid already exists in the
architecture: the host stays C++ and **Extensions** have a native authoring surface — native
extensions contribute Qt/QML panels (T3; the web mini-app/WASM sandbox is **dropped**,
`docs/architecture.md:51,209-218`). A high-level language belongs at the **Extension** seam, not the
host core, because the Extension surface is a separate native UI boundary — moving the host language
would not lower that cost.

## 3. Alternatives considered

Measured baseline: 16 crates / 84,370 Rust LOC. Verdicts sum to **port 1, re-derive 6, replace 4,
defer 3, drop 2** (`docs/rewrite-plan.md:69-86`). The 9 replace/defer/drop crates (**46,779 LOC,
~55%**) are re-authored regardless of language; the choice only affects the 7 logic crates
(**37,591 LOC, ~45%**).

| Candidate | Qt6 Quick/RHI | GES | Transliteration of 37.6k logic LOC | Runtime / bundle | GC / threading | Verdict |
|---|---|---|---|---|---|---|
| **C++23** (current) | first-party | first-party C API | same-family port/re-derive | none; links shipped libs | none | **Selected** |
| **Rust** | cxx-qt, immature; no proven RHI handoff | `gstreamer-rs` / GES crate, real but a second runtime | near-zero (source is Rust) | none; bundles cleanly | none | Strongest alternative; loses on Qt + R11 |
| **Ruby** | no maintained Qt6 binding | no meaningful GES binding | full re-derivation + FFI | interpreter + no bound libs | GC + GVL | Not viable as host |
| **Python** | PySide6 mature for QML; no supported render-thread RHI path | PyGObject/GI real; GES thinner | full re-derivation + FFI | CPython bundle; adds an interpreter | GC + GIL | Wrong for render-thread host; good as tooling |

### 3.1 C++23 (selected)

- **Pros.** First-party Qt Quick/QML and RHI (the render-thread path T4 needs); direct GES C API;
  same-family mapping from the 7 port/re-derive crates; no runtime to bundle; matches the engine
  adapter already specified in C++ (`docs/decisions/engine-choice.md:85-92`).
- **Cons.** Manual memory discipline (mitigated by RAII and the plan's preference for value/`const`
  style); no memory-safety guarantees of the kind Rust provides; the ecosystem for C++ video-editing
  hosts is thin compared with what Rust/FFmpeg tooling has accumulated. The GES quirks below are
  handled with direct calls, but they are C-API quirks, not language-specific.

### 3.2 Rust

Rust is the honest strongest alternative because Concat is Rust, so the 37.6k logic LOC would
transfer with near-zero cost, and `gstreamer-rs` plus a `gstreamer-editing-services` crate exist.

- **Pros.** Near-free transliteration of the logic core; memory safety; no GC; clean static bundling;
  the `gstreamer-rs` binding family is maintained.
- **Cons that decide it.** (a) **Qt Quick/RHI integration is the weak link.** `cxx-qt` targets Qt6 but
  is immature relative to C++, and there is **no established zero-readback texture handoff** from Rust
  to a Qt Quick render thread. The Phase 0 spike has since proven that handoff **in C++**
  (`docs/decisions/compositor-path.md`, 2026-09-29) — which sharpens this con rather than weakening
  it, since the proven arrangement (`QNativeInterface::QEGLContext` +
  `gst_gl_context_new_wrapped`) is C++-only API. (b) Adopting Rust for the host would either split UI and host
  across two languages or force a non-Qt UI, contradicting R1. (c) It **overturns R11**
  (`docs/architecture.md:44`) and the engine-choice §6 record, for a benefit (transliteration) that
  applies to ~45% of the source while the Qt boundary applies to 100% of the host's lifetime.
  A Rust **render/compositor** layer is a different question, deferred to §5 Q3.

### 3.3 Ruby

Ruby was the user's suggestion. It is not viable as the host core on the recorded constraints.

- **Transliteration.** Every one of the 37,591 logic LOC becomes a **full re-derivation**: no static
  types, no ownership model, no sum types to map, so behavior must be re-discovered and re-tested.
  The 9 replace/defer/drop crates are unaffected either way.
- **Qt6.** There is **no maintained Qt6 Quick binding** for Ruby; historical QtRuby/qtbindings stop at
  Qt5 and `ruby-qml` is unmaintained. R1/T4 cannot be met from Ruby.
- **GES.** GStreamer access would go through gobject-introspection (`gir_ffi` / ruby-gnome), whose
  GStreamer coverage is thin and whose GES coverage is effectively absent. The four recorded GES API
  constraints (`docs/dependencies.md:46-50,88-95`; plus the non-public `ges_clip_new` entry point
  encountered in the same spike phase) would have to be wrapped from an incomplete binding.
- **Real-time / distribution.** GC plus the GVL, and an interpreter to ship with no bound Qt/GES
  libraries. Not defensible for a three-platform desktop render-thread host.

Ruby would be a plausible language for a **scripted Extension** surface, but the Extension surface is
native Qt/QML and the web mini-app/WASM sandbox is **dropped**; Ruby is not a host-core candidate.

### 3.4 Python

Python is a better fit than Ruby for tooling and for driving GStreamer via GObject introspection, but
it is still the wrong side of the render boundary.

- **Transliteration.** Same as Ruby: the logic core is a full re-derivation, and every host↔engine and
  host↔UI crossing is an FFI/GI call.
- **Qt6.** PySide6 is a mature Qt for Python binding and can host QML. What is **not** a supported path
  is render-thread RHI integration with zero-copy texture handoff — the host would be orchestrating
  the render thread across the GIL.
- **GES.** PyGObject/GI exposes GStreamer well; GES via GI is thinner. The GES quirks still apply.
- **Real-time / distribution.** GC and the GIL: a per-frame call into a GIL-holding interpreter on the
  render thread forces cross-thread message passing or lock contention, complicating R4's worker
  boundaries. Bundling CPython on desktop is routine; embedding it in iOS/Android builds is restricted
  and adds size. Python is best kept for build tooling and, later, AI/analysis scripts — not the host.

## 4. Precise real-time position (do not overstate)

The host core runs on the UI thread and the render thread, but it is **not** the throughput
bottleneck. Decode, composite, and encode run in GStreamer, and pixel work runs in GPU shaders; the
host performs timeline math, command dispatch, and coordination — **microseconds per frame, not
milliseconds**. No candidate is disqualified by host arithmetic speed.

The real-time argument is therefore **architectural, not a speed claim**: C++ and Rust have no GC, so
per-frame coordination is deterministic; Ruby and Python introduce GC pauses and a GIL/GVL, so a
per-frame call from the render thread into the interpreter needs cross-thread handoff or lock
contention. That is a threading-model cost. It is cited as a secondary factor, not the reason C++
wins.

## 5. Consequences

**What this locks in.**
- R11 stands: host core, engine adapter, and tooling remain standard C++23 with no closed compiler
  extensions (`docs/architecture.md:44`). The host core stays portable and Qt-independent
  (`README.md:32`).
- The engine adapter is C++ and keeps GES types behind the boundary (R5/R6,
  `docs/architecture.md:38-39`); the MLT fallback is **dropped** (T2), so only GES crosses.
- Extensions remain a separate native UI surface (native extensions contributing Qt/QML panels; the
  web mini-app/WASM sandbox is **dropped**, T3), not a function of the host language.

**What this forecloses.**
- A single-language Rust host with Qt (the R11 reversal is not taken here).
- A scripting-language host core (Ruby/Python).
- Any expectation that the host language choice simplifies Extension authoring — the native
  Extension surface is independent of the host language.

**What would trigger a revisit (and reopen this record).**
1. ~~**Phase 0 chooses a custom RHI compositor** (`docs/rewrite-plan.md` §3(a), §7 Q5): if the render
   layer lands on Rust/wgpu, the host↔render boundary language is a new question.~~
   **Did not fire (2026-09-29):** Phase 0 passed with GES's compositor kept
   (`docs/decisions/compositor-path.md`), so the render layer does not land on Rust/wgpu and this
   trigger stays dormant.
2. **The team is predominantly Ruby/Python** and accepts the FFI + runtime cost: §5 Q1 is the only
   factor that could realistically overturn this decision.
3. **A maintained Qt6 + GES binding appears for a higher-level language**, removing the binding
   argument above.
4. **R11 is intentionally amended** by the user, e.g. to allow a different UI toolkit at T4.

## 6. Open questions

1. **Team composition.** The talent on the team and the languages they can staff a C++23/Qt/GES
   codebase in are unknown. This is the only factor that could realistically reverse the decision.
2. **Extension authoring language.** Does the user want a first-class scripting surface for
   Extensions on top of the native authoring UI? The web mini-app/WASM sandbox is **dropped** (T3,
   `docs/architecture.md:51,209-218`); this is separate from the host language and should be recorded
   separately.
3. ~~**Render-layer language under a custom compositor.** If Phase 0 selects a custom RHI compositor
   (`docs/rewrite-plan.md` §7 Q5), does the host stay C++ with a Rust render layer, or does the whole
   boundary move?~~
   **Closed (2026-09-29):** Phase 0 kept GES's compositor (`docs/decisions/compositor-path.md`), so
   there is no separate render-layer language to choose. Reopens only if a revisit trigger fires.
4. **R11 intent.** Confirm that "no closed compiler extensions" refers to compiler dialects (not to
   code generators such as Qt's `moc`), so Qt's standard build remains compliant. Read as written it
   is satisfied by standard C++23 plus `moc`.

---

## 7. Prior art: OpenShot

The user pointed at OpenShot (`github.com/OpenShot/openshot-qt`, `src/classes`, Python) as evidence
that a video editor can be written in a scripting language. The premise is half right, and the half
that is right does not reopen this decision.

**OpenShot is two layers, and only one is Python.**

- `openshot-qt` — **Python** (PyQt5): the GUI and controller. Windows, the timeline widget, menus,
  project data, threading glue. This is the `src/classes` tree.
- `libopenshot` — **C++**: the engine. Decoding, frame rendering, compositing, transitions, and
  encoding, over FFmpeg. `libopenshot-audio` is C++ as well.
- The Python layer reaches the engine through **SWIG bindings**; `src/classes/timeline.py`,
  `project_data.py`, `clip_utils.py`, and `keyframe_scaler.py` are wrappers over `libopenshot`'s C++
  objects plus Qt plumbing. `logger_libopenshot.py` names the dependency outright.

So OpenShot is **a Python shell over a C++ engine** — the hybrid pattern, not a scripting-language
engine. No file in `src/classes` decodes or renders pixels; that is `libopenshot`'s work, and its own
`Timeline` object does the per-frame coordination. PyQt was chosen for velocity and cross-platform Qt
GUI reach; the engine was always going to be native.

**Mapping onto Genesis-0.**

| OpenShot | Genesis-0 | Notes |
|---|---|---|
| `openshot-qt` Python GUI/controller | Qt6 Quick/QML (R1) | the high-level shell |
| `src/classes/*.py` document model driving the UI | QML controller, **not** the host core | `libopenshot`'s C++ `Timeline` owns per-frame work |
| `libopenshot` (C++) | GStreamer/GES (T1) | the native engine |

The Python layer holds the document model and drives the UI, but it is **not on the render path**. In
Genesis-0 terms it is closer to the **QML-controller** role than to the **host-core** role, which is
why it does not contradict §2: the code Genesis-0 places in C++ (timeline math, command dispatch,
render coordination) is `libopenshot`'s C++ work in OpenShot, not the Python layer's.

**What OpenShot pays for the split.** Per-operation SWIG crossings between Python and C++ (a known
source of sluggishness on large projects); a bundled CPython + PyQt + compiled `libopenshot` in every
package, with a correspondingly heavy mobile story; and GIL constraints on anything the Python layer
does concurrently.

**Verdict.** OpenShot validates two things Genesis-0 already holds: the engine must be native (T1),
and a high-level shell belongs at the **UI/Extension seam**, not the host core. It does not show that
the host core can be Python. It reinforces §3.4 (Python for tooling, wrong for a render-thread host)
and the hybrid paragraph of §2.

---

## Evidence index

| Fact | Location |
|---|---|
| Host core standard C++23; no closed compiler extensions | `docs/architecture.md:44` (R11) |
| Engine types never cross the adapter boundary | `docs/architecture.md:38-39` (R5/R6) |
| GES primary; MLT fallback dropped (T2); mobile out-of-repo | `docs/architecture.md:49-50,60`; `docs/decisions/engine-choice.md:15-22` |
| Qt RHI target | `docs/architecture.md:52` (T4) |
| Extension/pack terminology; native extension UI not the host language | `docs/architecture.md:11-16,51,209-218` |
| Host owns rational time and command undo | `docs/architecture.md:145-151` |
| No C++ ABI stability promise | `docs/architecture.md:217` |
| Rewrite is re-derivation, not translation | `docs/rewrite-plan.md:42-47` |
| 16 crates; per-crate LOC; verdicts | `docs/rewrite-plan.md:69-86,553-556` |
| Rust source 84,370 lines / 132 files | `docs/rewrite-plan.md:27-30,463` |
| Qt-Quick render risk (zero-readback handoff unproven) | `docs/rewrite-plan.md:210-215` |
| Host model in C++; adapter boundary | `docs/decisions/engine-choice.md:85-92` |
| GPL-3.0 incorporation of Concat (code source) | `docs/decisions/licensing.md`; `docs/decisions/engine-choice.md:108-112` |
| Qt6 6.11.2 / GStreamer 1.28.6 / GES installed | `docs/dependencies.md:20-30` |
| GES quirks: no `.pc`; `glshader` varyings; `ges_asset_request` sync null | `docs/dependencies.md:46-50,88-95` |
| OpenShot = Python/PyQt GUI over C++ `libopenshot` engine (prior art, §7) | `github.com/OpenShot/openshot-qt` `src/classes`; `github.com/OpenShot/libopenshot` |

See `docs/architecture.md` for the decision register, `docs/rewrite-plan.md` for the crate mapping
and phases, and `docs/decisions/engine-choice.md` / `docs/decisions/licensing.md` for the engine and
license decisions this record is constrained by.
