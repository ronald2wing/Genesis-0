# Genesis-0 — Architecture

Status: implemented. The host core, GES adapter, and Qt Quick desktop app are built. The marketplace
section remains planned. AI sections are implemented where noted (gated runtimes). Checked: 2026-10-05.

The engine choice is in [engine-choice.md](decisions/engine-choice.md).

## System map

### Feature surfaces

The product has five kinds of surface. Each is parsed from one source of truth, so the index and
the code cannot disagree:

| Kind | Source of truth | Count | What it is |
|---|---|---|---|
| API verbs | `src/api/Requests.cpp` | 41 | The JSON-RPC methods (`edit.apply`, `export.run`, `ai.run`, …). |
| CLI verbs | `src/cli/main.cpp` | 19 | `genesis-cli` subcommands, including nested ones (`render`, `extensions install`, `config`, …). |
| Command verbs | `src/cli/Commands.cpp` | 45 | The project edit commands (`AddClip`, `SetClipSpeed`, …) carried by `edit.apply`. |
| QML surfaces | `src/app/qml/*.qml` | 19 | The UI: the shell, panels, dialogs, and design-system widgets. |
| Controllers | `src/app/controllers/*.h` | 22 | The C++ objects the QML binds to, one class per file. |

The targets that hold them: `genesis_host` (engine-free, Qt-free core), `genesis_ai` (AI providers),
`genesis_engine_ges` (the only library linking GStreamer/GES), `genesis_app` (the only Qt target),
and `genesis_cli` (the headless CLI). Per-target detail is in `AGENTS.md` at the repo root.

### Debug loop

When something is wrong, `where` turns a symptom into a plan:

1. **Name the symptom.** `genesis-dev where <word>` — e.g. `export`, `timeline`, `captions`.
2. **Read the match.** Each hit gives its `file:line`, the suites that pin it, the smoke surface
   that exercises it, and a runnable command.
3. **Reproduce.** Run the printed command. For a UI surface, `genesis-dev smoke --surfaces <name>`
   opens it offscreen and fails on any QML runtime message. For a verb, drive it headlessly with
   `genesis-cli api` (JSON-RPC over stdio) or `genesis-cli <verb>`.
4. **Pin the fix.** Run the owning suite: `ctest --test-dir build -R <suite> --output-on-failure`.

Worked examples:

- **"Export fails."** `genesis-dev where export` → `export.run` (api-verb), `ExportController`
  (controller, suite `export_controller`, smoke surface `export`), `render` (cli-verb). Reproduce
  with `genesis-dev smoke --surfaces export`, then `ctest -R export_controller`.
- **"A QML error in settings."** `genesis-dev where settings` → `SettingsDialog` (qml-surface,
  smoke surface `settings`). Reproduce with `genesis-dev smoke --surfaces settings`; the gate
  prints the QML message.
- **"An AI run is stuck."** `genesis-dev where ai` → `ai.run`/`ai.status` (api-verbs),
  `AiController` (suite `ai_controller`). Reproduce with `genesis-cli api` and the `ai.*` methods,
  then `ctest -R ai_controller`.

The smoke gate opens surfaces against a fresh, empty project, so a bug that needs a loaded
project plus a selection may not reproduce there — fall back to the full `ctest` run or a manual
load-project exercise.

## Planning content

The sections below are the original planning record: the decision register, scope, and the
architecture rationale. They are kept for history and for the *why* behind the shape.

**Scope (2026-10-03).** This repository is **desktop-only** — Windows, macOS, Linux. Mobile
(Android/iOS) editing is **out of scope for this repository** and is planned in a **separate
repository (future)**. Mobile notes retained below are historical/re-scoped context, not work
planned here.

Genesis-0 is a modern, performant video editor *inspired by* CapCut-class workflows. It does not
copy CapCut branding or assets. It targets desktop **Windows, macOS, Linux**. "All OS" here means
these three desktop families — not every distribution, version, device, or web target.

**Terminology.** The umbrella term is **Extension**; its two kinds are **Packs** (declarative
data/GLSL effect bundles) and **native extensions** (shared libraries contributing native code/UI;
the public **Extension SDK**). The word *plugin* is reserved for technical frameworks and their
objects — Qt plugins, GStreamer/GES plugin elements, and the manifest `kind` field; *module* and
*mini-app* are not used for this system. An Extension may be built on those plugin mechanisms, but
the public contract never exposes them (R6). Existing technical references to GES plugin names
are kept as-is.

---

## 1. Scope and non-goals

In scope (eventual): adaptive desktop editing, a real third-party *functional* extension ecosystem
with one-click marketplace install, masks/keyframe rotoscoping/motion tracking, and a future AI path
(text/image-to-video plus analysis). Portable project files across desktop platforms. Mobile is out
of scope for this repository (separate repo, future).

Non-goals for this document: no code, no scaffolding, no model or dependency installation, no
store submission, no byte-identical export promise, no universal-60fps promise, no effort estimate.

## 2. Decision register

### 2.1 Settled requirements
| # | Requirement |
|---|---|
| R1 | Adaptive Qt6 Quick/QML shell; desktop workspace. Phone/tablet adaptive shell is out-of-repo (separate mobile repository, future). |
| R2 | Width **and** height/input-mode used together; shortest-dimension breakpoints are rejected (landscape misclassification). |
| R3 | Touch targets ~44–48 logical units; accessibility, focus, keyboard, reduced motion, safe areas. |
| R4 | Native fast core with worker boundaries; **no media work on the UI thread**; virtualized thumbnails/waveforms/proxies. |
| R5 | Host owns project/domain/effect graph and rational time; adapters compile to the engine (host does not re-implement codecs). |
| R6 | Plugin/project APIs never expose GES objects. |
| R7 | Marketplace is a signed, versioned pipeline with compatibility, consent, atomic install, health check, rollback. |
| R8 | Third-party plugins add **real new functionality**; declarative presets/control schemas alone do not satisfy R8. |
| R9 | Missing/unsupported plugin effects keep the project intact; affected exports warn/block unless the user explicitly bypasses or uses a saved render. |
| R10 | AI jobs are async with submit/status/progress/cancel/result; generation adds nondestructive clips; analysis adds masks/tracks/captions; source is never overwritten. |
| R11 | Host core, engine adapter, and tooling use standard **C++23** (no closed compiler extensions required); the UI shell remains Qt6 Quick/QML. |

### 2.2 Tentative choices (revisable after spikes)
| # | Choice | Gate to keep it |
|---|---|---|
| T1 | **GStreamer + GES as the primary engine** — the only surveyed candidate with a timeline model (GESTimeline/Layer/Track/Clip/Transition/Project), a plugin ABI, and LGPL. Recorded in `docs/decisions/engine-choice.md`. | Desktop render-into-Qt-Quick spike **passed (2026-09-29)**; the device playback/seek/timeline/composite/export spike is desktop-only and still unrun (mobile is out-of-repo). |
| T2 | ~~**A desktop fallback engine.**~~ **Dropped (2026-10-03)** — the desktop GES path works; no fallback engine is planned. Historical context retained in `docs/decisions/engine-choice.md` §3. | n/a — fallback dropped. |
| T3 | ~~One sandbox/worker approach for executable third-party logic (web mini-app vs WASM).~~ **Dropped (2026-10-03)** — the web mini-app/sandbox option is removed; extension UI is native (Qt/QML panels contributed by native extensions). | n/a — sandbox dropped. |
| T4 | Qt RHI for rendering. | Prototype vs QQuickFramebufferObject (legacy GL-only). |
| T5 | Static curated catalog + object storage initially. | No payments or large backend required. |

### 2.3 Unresolved decisions (must not be assumed closed)
- **Engine gates**: the primary engine (GES) choice is recorded in `docs/decisions/engine-choice.md`.
  The **GES desktop render-into-Qt-Quick spike passed (2026-09-29)** — GES output reaches the Qt Quick
  scene graph zero-copy (`docs/decisions/compositor-path.md`); the device/desktop tests are unrun, and
  **no performance claims** are made. No fallback engine is planned (see T2); mobile is out-of-repo.
- **Product license**: recorded in `docs/decisions/licensing.md` — first-party code
  GPL-3.0-or-later, reference code incorporated under GPL-3.0, closed-source/paid Extensions no longer
  supported (a plugin/linking exception can be added later). Recorded as a product/engineering decision; **qualified
  legal review remains pending** and no legal/store approval is claimed [S6].
- Minimum supported OS versions and reference devices (must be chosen before benchmarks mean anything).
- Final AI model/provider selection, local vs cloud default, and cost/privacy posture.

## 3. Corrections to earlier advice

| Earlier claim | Correction | Source |
|---|---|---|
| "Default marketplace has no signing." | VS Code's real marketplace does sign and the client verifies; **signature != sandbox**. | [S7] |
| "Widgets/QQuickWidget are impossible on mobile." | Not asserted. We *prefer* pure Quick architecturally, but static `Q_IMPORT_PLUGIN` and dynamic bundled native plugins are possible. | [S3] |
| "All QML is AOT-compiled, so iOS is fine." | QML can run interpreted bytecode; AOT is an optional optimization, not mandatory. | [S3] |
| "Texture uploads are zero-copy automatically." | Zero-copy is a *measured capability*, not an assumption — texture/fence handling is explicit. | [S2] |
| "WASM/QML is blanket store-approved / blanket impossible." | Neither. Apple 4.7 permits certain nonbundled mini-apps under strict rules; Google Play restricts downloaded .so/dex/JAR with a conditional VM/interpreter exception. Actual bridge/store eligibility is a gate. | [S4][S5] |

## 4. Architecture overview

```
+---------------------------------------------------------------+
|  Qt6 Quick/QML adaptive shell (desktop; shared tokens/components) |
|  desktop workspace                                                |
+---------------------------------------------------------------+
        |  host-defined declarative control schemas (data, not code)
        v
+---------------------------------------------------------------+
|  Host core (native C++23, worker-threaded)                     |
|  project/domain model | rational time | effect graph           |
|  undo/redo commands | asset IDs/storage | document versioning |
+------------------+--------------------------------------------+
                   |  engine adapter (compiles graph -> pipeline)
                   v
+---------------------------------------------------------------+
|  Media engine (GES)                                           |
|  decode | composite | timeline | encode | export              |
+---------------------------------------------------------------+
        ^                                   ^
        |                                   |
+-----------------------+          +---------------------------+
| Extension host        |          | AI job runtime            |
| native panels/dialogs |          | desktop workers /         |
| marketplace trust     |          | in-process or cloud       |
+-----------------------+          +---------------------------+
```

Host project files are the source of truth. Engine pipelines, OTIO, and plugin packages are
**derived or optional interchange** — OTIO is optional interchange only, *not* a guaranteed
lossless host project format.

## 5. Adaptive UI shell (Qt6 Quick/QML)

- Shared component/token layer: spacing, type scale, color roles, motion curves, focus rings.
- Desktop: assets / preview / inspector / timeline workspace.
- Phone/tablet adaptive shell (bottom sheets, compact toolbars, split panes) is **out-of-repo**
  (separate mobile repository, future).
- **Breakpoint rule:** classify by available width *and* height plus input mode. Do not use
  shortest-dimension breakpoints (a landscape phone would be misread as desktop).
- Touch targets ~44–48 logical units; safe-area insets; keyboard/focus traversal; reduced-motion
  honoring; screen-reader labels on interactive controls.
- Rendering: prototype Qt RHI (Vulkan/Metal/D3D/GL) [S2]. `QQuickFramebufferObject` is legacy
  GL-only [S2] — retained only as a comparison baseline, not the target.
- No desktop Widgets dependency required; a pure Quick architecture keeps the UI stack single for
  the desktop shell [S3].

## 6. Engine strategy

- **GES is the primary engine**, recorded in `docs/decisions/engine-choice.md`: it has a timeline
  model (GESTimeline/Layer/Track/Clip/Transition/Project) [S1], a plugin ABI, and LGPL. This is
  **not** evidence of app performance.
- Before locking the engine or freezing a public SDK, run device tests for: media playback, seek,
  timeline manipulation, compositing, and export on desktop. The **GES desktop render-into-Qt-Quick
  spike passed (2026-09-29)** (`docs/decisions/compositor-path.md`); the broader tests remain unrun.
- ~~**A desktop fallback engine** if GES fails on desktop.~~ **Dropped (2026-10-03)** — the
  desktop GES path works and no fallback engine is planned. Historical context is retained in
  `docs/decisions/engine-choice.md` §3.
- Mobile (native AVFoundation/Media3 adapters) is **out of scope for this repository**; it is planned
  in a separate mobile repository (future). See `docs/decisions/engine-choice.md` §4 for historical
  context.
- Engine swapping and effect translation are **not free**; the adapter boundary absorbs that cost.
  The host owns the portable project model and never leaks GES types to UI or Extensions (R5/R6).

## 7. Host project / domain model

- Rational time everywhere (no float seconds for edit points).
- Undo/redo expressed as commands over the domain model.
- Assets referenced by stable IDs + relocatable paths (portable projects).
- A version number so newer files are refused whole on older builds (never half-loaded).
- Engine adapter compiles the effect graph into an engine pipeline; it does **not** re-implement codecs.

## 8. Preview / render / export consistency

- Consistency contracts cover proxies, original-source export, color/alpha/audio, fps, and timebase.
- Proxies never leak into final export; export always uses original sources.
- Extension state persists as `{extension_id, version, parameters}`. Unsupported/missing effects are
  marked. Project state is preserved; affected exports warn or block unless the user explicitly
  bypasses or uses a previously saved render (R9).
- Reproducible semantics are defined as frame/audio tolerance **on a fixed version and fixed
  resources** — **not** byte-identical exports across different hardware or codec builds.
- Hardware decode/encode and zero-copy are measured capabilities, not assumptions [S2].

## 9. Extension architecture and marketplace manifest

Stable manifest/package contract fields: schema/API version ranges; id/publisher/version; kind and
entry points; platform + arch + runtime targets; named capability/permission *reasons*; media
input/output schemas; hashes/signature trust; model/data license; optional variants.

Illustrative manifest (small, valid JSON; not a production SDK):

```json
{
  "schemaVersion": "1.0",
  "id": "com.example.smart-blur",
  "publisher": "Example Labs",
  "version": "1.2.0",
  "apiVersionRange": ">=1.0 <2.0",
  "kind": "effect",
  "entryPoints": [{ "runtime": "native", "path": "libsmartblur.so" }],
  "targets": [{ "os": "windows", "arch": "x64" }, { "os": "linux", "arch": "x64" }],
  "permissions": [{ "name": "read-media", "reason": "Reads selected clip frames to apply blur." }],
  "media": { "input": ["video/rgba8"], "output": ["video/rgba8"] },
  "integrity": { "sha256": "0000000000000000000000000000000000000000000000000000000000000000" },
  "license": { "code": "MIT", "modelData": null }
}
```

Interface sketches (illustrative only, not a production SDK):

```
interface EffectExtension {
  descriptor(): ExtensionDescriptor;              // id, version, apiVersion
  parameters(): ControlSchema;                    // host-rendered declarative controls
  process(frame: FrameRef, params: Json): FrameRef;
}
interface AiJob {
  submit(req: JobRequest): JobId;
  status(id: JobId): JobState; progress(id: JobId): number; cancel(id: JobId): void;
}
```

- Control schemas are **host-defined declarative data**, compiled to portable widgets. Arbitrary
  QML/JS is **not** "safe data" and is not accepted as an extension control payload.
- Presets/assets may only configure already-shipped operations; they do **not** satisfy R8 alone.
- Plan includes a true desktop extension SDK (native shared library) and **at least one independently
  packaged example** adding a genuinely new command/filter/tool. Extension UI is **native** (Qt/QML
  panels contributed by native extensions); the web mini-app sandbox is dropped (§10).

## 10. Extension sandbox decision (dropped)

- **The sandbox/web-mini-app option is dropped (2026-10-03).** Extension UI is **native** (Qt/QML
  panels contributed by native extensions); no web mini-app or WASM sandbox runtime is planned (T3).
- WASM is not automatic store approval and not perfect security. The shared QML engine is **not**
  a sandbox.
- Native GPU/media extensions are a trusted, curated desktop-only path unless isolated via **measured**
  IPC. A signed package is not "safe"; a process boundary alone is not a sandbox.
- No Qt/C++ ABI stability promise is offered. Extensions needing native code declare ABI/Qt/toolchain.
- Native unloading may require a restart; arbitrary hot-swap is not promised.

## 11. Marketplace install pipeline

`catalog -> compatibility -> consent -> download to temp -> verify signature / trusted identity /
content digests -> safe extraction (traversal, symlink, zip-bomb, size limits) -> atomic versioned
installation -> activation + health check -> rollback`.

- Updates repeat compatibility, permissions, and re-consent.
- Active project dependency versions are **never** mutated mid-session.
- Uninstall/revocation must preserve projects and media. Signed-catalog revocation and key rotation
  are planned.
- Developer workflow: validate -> package -> submit -> review -> publish. Static curated catalog +
  object storage initially; no payments or large backend (T5).
- Install-from-file uses the **same** pipeline; developer exceptions are visibly separate.

## 12. AI job / artifact contract

- Async contract: submit / status / progress / cancel / result.
- Request carries input range, asset + timebase, parameters, and output metadata.
- Generation adds new nondestructive clips; analysis supplies masks/tracks/captions; the source is
  never overwritten (R10).
- Desktop uses AI workers. Mobile AI (in-process or cloud) is **out-of-repo** (separate mobile
  repository, future).
- GPU/VRAM admission, concurrency, and chunking preserve preview responsiveness. Pausing a job does
  not necessarily free VRAM.
- Local vs cloud has explicit privacy/consent/credentials/cost/model-license/provenance handling and
  download digest checks. This section originally selected or installed no specific AI models; the
  host now ships a gated model catalogue and store (`src/ai/ModelManifest.cpp`,
  `src/ai/ModelStore.cpp`), so models are obtainable where the runtime options are ON.

## 13. Security and trust

- Package trust: signature + trusted identity + content digests, enforced in the pipeline (§11).
- Hostile-package defenses: path traversal, symlinks, zip bombs, oversized payloads.
- Permission model enumerates named capabilities with human-readable reasons; updates re-consent.
- Crash recovery and cancellation are designed into job and extension lifecycles.

## 14. UX

Portable project files open on any desktop platform. Missing extensions degrade with clear
markers; the user can install, substitute, or export a saved render. Long jobs show progress and
are cancellable. Permission prompts explain the *reason*, not just the name.

## 15. Performance gates (proposed, not achieved)

These are **budgets to be validated**, not results. Fixtures and reference devices are chosen first.

- 1080p30 playback over 60 s: dropped frames <= 1%.
- p95 input response <= 100 ms.
- No universal 60 fps promise; thresholds are device- and fixture-specific.

## 16. Legal / licensing gates

- GPL is commercially usable, but dynamic linking does not evade GPL obligations; static LGPL is
  possible with obligations [S6].
- Qt open-source/LGPL obligations have store/relink implications requiring qualified review [S6].
- Product license decision is **recorded** (first-party GPL-3.0-or-later; reference code incorporated
  under GPL-3.0; closed-source/paid Extensions **no longer supported**, a plugin/linking exception
  may be added later) in `docs/decisions/licensing.md`. This is a product/engineering record, **not
  legal or store approval**; qualified counsel review remains a gate. Commercial Qt does **not** solve
  third-party GPL/codec/patent licensing. GPL-3.0-or-later has no network-source obligation, but its
  copyleft still makes App Store / Play distribution a **hard gate**.
- No blanket OS-license matrix. Licensing review happens **early**, before incorporating any GPL
  module.

## 17. Platform capability matrix (conditional — not approval ticks)

Desktop-only repository. Android/iOS columns are **out-of-repo** (separate mobile repository, future)
and are not planned here; they are omitted rather than ticked.

| Capability | Windows | macOS | Linux |
|---|---|---|---|
| Desktop workspace UI | planned | planned | planned |
| Engine (GES primary) | spike | spike | spike |
| Native extension (bundled shared library) | planned | planned | planned |
| Downloaded executable extension | spike | spike | spike |
| AI worker runtime | planned | planned | planned |

Every cell is a hypothesis requiring validation; none is a green approval tick.

## 18. References

Checked 2026-09-28. Source IDs map to claim tables above. The appendix covers **12 distinct URLs**
across 7 source IDs; each ID groups the URLs supporting the numbered verified facts in the task.

| ID | Source | Local path / line |
|---|---|---|
| S1 | GStreamer GES docs; GStreamer Android/iOS deployment guides — `https://gstreamer.freedesktop.org/documentation/gst-editing-services/index.html`, `/installing/for-android-development.html`, `/installing/for-ios-development.html` | — |
| S2 | Qt RHI visual canvas; QQuickFramebufferObject — `https://doc.qt.io/qt-6/qtquick-visualcanvas-adaptations.html`, `https://doc.qt.io/qt-6/qquickframebufferobject.html` | — |
| S3 | Qt QML script compiler; Qt plugin how-to — `https://doc.qt.io/qt-6/qtqml-qml-script-compiler.html`, `https://doc.qt.io/qt-6/plugins-howto.html` | — |
| S4 | Apple App Store Review Guidelines 2.5.2, 4.7, 4.7.1–4.7.2 — `https://developer.apple.com/app-store/review/guidelines/` | — |
| S5 | Google Play downloaded-code policy — `https://support.google.com/googleplay/android-developer/answer/9888379` | — |
| S6 | GNU GPL FAQ; Qt open-source LGPL obligations — `https://www.gnu.org/licenses/gpl-faq.html`, `https://www.qt.io/licensing/open-source-lgpl-obligations` | — |
| S7 | VS Code extension marketplace (signing + client verification) — `https://code.visualstudio.com/docs/configure/extensions/extension-marketplace` | — |

See `docs/history/implementation.md` for ordered phases, gates, and acceptance tests.
See `docs/decisions/engine-choice.md` for the engine decision (GES primary; no fallback engine;
mobile native adapters out-of-repo, with an external source appendix).
See `docs/decisions/licensing.md` for the licensing decision register and `docs/dependencies.md` for
dependency status, versions, and license evidence.
