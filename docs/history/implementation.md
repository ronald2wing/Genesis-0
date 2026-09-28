# Implementation Plan (Historical Record)

> Status: historical. This plan was executed. It is a record of intent, not a description of
> the current tree. Do not read as current design.

**Re-scope (2026-10-03).** This repository is **desktop-only**; mobile (Android/iOS) is out of scope
and planned in a separate repository (future). No fallback engine is planned, and the
sandbox/web-mini-app option has been dropped (see `docs/decisions/engine-choice.md`,
`docs/architecture.md` §10). Mobile items below are retained as historical context and are marked
out-of-repo.

Companion: `docs/architecture.md`.

---

## 0. How to read this plan

- Phases are ordered; **gates** must pass before later phases consume their output.
- Each phase lists: scope, planned artifacts (future paths), dependencies, verification owner role,
  acceptance tests, and stop/fallback criteria.
- Independent tasks may run in parallel **only when no gate is crossed**.
- Verification owner roles are role names, not people. Implementation is not run here.
- Terminology follows `docs/architecture.md` §1: the umbrella term is **Extension**, with kinds
  **Pack** (declarative) and **native extension** (shared library); *plugin* is used only for
  technical frameworks (Qt, GStreamer/GES elements) and manifest internals.

## Phase 0 — Product/license choices + benchmark fixtures

**Scope:** record the license decision; define fixtures and reference hardware before any performance
number exists.

**Planned artifacts**
- `docs/decisions/licensing.md` (**recorded**: first-party GPL-3.0-or-later; reference code incorporated
  under GPL-3.0; closed-source/paid Extensions no longer supported; Qt/dependency posture) [S7] — see
  `docs/decisions/licensing.md` and `docs/dependencies.md`
- `docs/decisions/minimum-platforms.md (not created)` (minimum OS versions, reference devices)
- `benchmarks/fixtures/` (media fixtures: 1080p30, 4K, alpha, variable fps, long-GOP)
- `benchmarks/reference-devices.md` (desktop ×2; mobile reference devices are out-of-repo)

**Dependencies:** none. **Owner:** product + legal review + performance lead.

**Acceptance tests**
- Licensing decision recorded in `docs/decisions/licensing.md`; **qualified counsel review still
  pending** — a written record is not legal or store approval [S7].
- Fixtures and reference devices chosen; fixture checksums recorded.
- Budgets defined: 1080p30 playback 60 s drops <= 1%; p95 input response <= 100 ms.

**Stop/fallback:** if license cannot be resolved, keep the decision open and continue spikes that do
not incorporate GPL modules.

## Phase 1 — Desktop engine + render spikes

**Scope:** validate GES (primary, T1) on desktop hardware; prototype Qt RHI. The fallback engine and
mobile were re-scoped out (2026-10-03).

**Planned artifacts**
- `spikes/engine-ges/` (playback, seek, timeline, composite, export probes)
- `spikes/render-rhi/` (RHI vs QQuickFramebufferObject comparison) [S3]
- `docs/decisions/engine-choice.md` (**recorded**: GES primary; no fallback engine)

**Dependencies:** Phase 0 fixtures/devices. **Owner:** engine spike lead.

**Acceptance tests**
- Playback/seek/timeline/composite/export measured on **real desktop hardware**.
- Zero-copy/hardware decode treated as a measured capability, not assumed [S3].
- Mobile deployment/validation is **out-of-repo** (separate mobile repository, future) [S1].

**Stop/fallback:** if GES fails the desktop gate, escalate as a product decision — no fallback
engine and no second engine is planned (`docs/decisions/engine-choice.md`).

**Gate:** public extension/project API freeze waits for this phase; APIs never expose GES objects.

## Phase 2 — Host project model + desktop editor slice

**Scope:** host-owned domain/effect graph, rational time, undo, adapter — standard **C++23** (R11).

**Planned artifacts**
- `core/project/` (schema, rational time, migrations, capabilities)
- `core/effects/` (effect graph)
- `core/undo/` (command stack)
- `adapters/engine/` (compiles graph -> engine pipeline; no codec reimplementation)
- `ui/qml/` (shared tokens/components; desktop workspace)

**Dependencies:** Phase 1 engine choice. **Owner:** core lead + UI lead.

**Acceptance tests**
- Effect graph round-trips via engine adapter on both tested OSes.
- No media work on the UI thread; thumbnails/waveforms/proxies virtualized [R4].
- Undo/redo restores graph state exactly.

**Stop/fallback:** adapter contract proven on one engine before a second is ever considered.

## Phase 3 — Masks / keyframe rotoscoping / motion tracking

**Scope:** host-owned mask/tracker model with engine bridge; validate GES-side tooling.

**Planned artifacts**
- `core/masks/` (Bézier spline model, keyframes)
- `core/tracking/` (track data, analysis bridge)
- `ui/qml/inspector/mask/`

**Dependencies:** Phase 2. **Owner:** effects lead.

**Acceptance tests**
- A tracking box is bridged to spline/mask **outside** the extension — box ≠ tracked
  spline/mask; UI + transform/analysis bridge is built, not assumed.
- If masks are chosen for GES, the choice is validated on desktop.
- Mask boundary tests: color/frame-accuracy on alpha and edges.

**Stop/fallback:** if GES lacks a viable mask path, keep masks host-side or gate the feature;
do not claim any GPL tracking/degradation modules are license-free [S7].

## Phase 4 — Signed marketplace + native extension SDK

**Scope:** manifest contract, install pipeline, one real example native extension.

**Planned artifacts**
- `marketplace/schema/manifest.schema.json`
- `marketplace/pipeline/` (catalog -> compatibility -> consent -> temp -> verify -> safe extract ->
  atomic install -> health check -> rollback)
- `extensions/sdk/` (desktop **Extension SDK**; public name, internal path may vary)
- `extensions/example-smart-blur/` (**independently packaged example adding new functionality**)
- `tests/security/hostile-packages/`

**Dependencies:** Phase 2. **Owner:** marketplace lead + security reviewer.

**Acceptance tests**
- Extension UI is **native** (Qt/QML panels from native extensions); the web mini-app/WASM sandbox
  option is **dropped** (2026-10-03).
- Hostile package tests: path traversal, symlink, zip bomb, oversized payload, corrupt signature ->
  rollback; permission denial and update escalation -> re-consent.
- Missing-extension portability/export test passes (R9).
- Example extension demonstrably adds a new command/filter/tool (R8); declarative presets alone
  rejected.
- Signature verified but **not** equated with safety.

**Stop/fallback:** if the native extension path cannot be made safe enough, restrict executable
extensions to the curated desktop path; no sandboxed/mobile executable path is planned.

## Phase 5 — Mobile full workflow (out-of-repo)

**Re-scoped (2026-10-03):** mobile (phone/tablet workflow and the mobile extension path) is **out of
scope for this repository** and is planned in a **separate mobile repository (future)**. Retained
historically:

- `ui/qml/mobile/` (bottom sheets, toolbars) — out-of-repo
- `docs/decisions/mobile-plugin-route.md (not created)` (Apple 4.7 bridge vs bundled native; Play rules) — out-of-repo

**Acceptance tests:** breakpoints/touch-safe-area/reduced-motion and cross-platform project
portability are validated in the mobile repository, not here.

## Phase 6 — AI provider/artifact demonstration

**Scope:** async job contract with one demonstrated provider path.

**Planned artifacts**
- `core/jobs/` (submit/status/progress/cancel/result)
- `ai/providers/` (desktop worker; mobile in-process/cloud is out-of-repo)
- `docs/decisions/ai-provider.md` (local vs cloud, privacy/consent/cost/license/provenance)

**Dependencies:** Phase 2. **Owner:** AI integration lead + privacy reviewer.

**Acceptance tests**
- Generation adds nondestructive clips; analysis adds masks/tracks/captions; source never
  overwritten.
- Cancellation and crash recovery verified.
- GPU/VRAM admission/concurrency/chunking preserves preview; pausing is not assumed to free VRAM.
- Download digest checks and model/data license recorded. No specific models selected in this plan.

**Stop/fallback:** if no provider satisfies privacy/license, ship the job contract with a mock
provider and keep model selection open.

## Phase 7 — Release hardening

**Scope:** packaging, CI matrix, signed releases, device perf, legal. Desktop-only (2026-10-03);
mobile packaging/CI belongs to the separate mobile repository (future).

**Planned artifacts**
- `ci/matrix.yml` (three desktop OS families; Windows, macOS, Linux. Android/iOS are **out-of-repo**)
- `packaging/` (minimal initial packaging per OS — **not six Linux installers**)
- `docs/security/key-management.md (not created)` (signing keys stored externally)
- `docs/legal/licensing-review.md (not created)`

**Dependencies:** Phases 0–6. **Owner:** release engineer + security + legal.

**Acceptance tests**
- CI matrix builds the three desktop OS families; **unsigned CI artifacts** clearly distinct from
  **signed release** artifacts; secret signing keys kept external.
- Device performance tests run on reference hardware, **not** emulators.
- License review completed **before** any GPL module ships [S7].
- Cross-links/JSON examples in both docs validated.

**Stop/fallback:** if a platform cannot meet the performance budget, ship it with an explicit
capability note rather than a universal-performance claim.

## CI matrix and packaging

| OS family | CI build | Minimal initial packaging |
|---|---|---|
| Windows | yes | one installer |
| macOS | yes | one app bundle |
| Linux | yes | one AppImage/flatpak-style package (not six) |
| ~~Android~~ | out-of-repo | moved to the separate mobile repository (future) |
| ~~iOS~~ | out-of-repo | moved to the separate mobile repository (future) |

Emulators/simulators are for functional checks only; **performance gates run on real desktop
devices**. Mobile CI/packaging is not planned in this repository.

## Documentation acceptance checklist

The unchecked boxes below are a historical record of the checklist as first
drafted, not open work — both docs exist and are maintained.

- [ ] Both docs exist: `docs/architecture.md`, `docs/history/implementation.md`.
- [ ] Cross-links (`architecture.md <-> history/implementation.md`) resolve.
- [ ] Manifest JSON example parses and is internally consistent.
- [ ] Phase IDs are consistent and ordered; every phase has an explicit acceptance gate.
- [ ] Settled requirements vs tentative choices vs unresolved decisions are separated.
- [ ] Reference appendix has 10–16 references grouped under source IDs, with checked date 2026-09-28.
- [ ] No release/performance/compliance claim; no file/module/install action executed.

## Open questions (do not block the docs)

1. Product license: **recorded** (first-party GPL-3.0-or-later; reference code incorporated under
   GPL-3.0; closed-source/paid Extensions no longer supported) in `docs/decisions/licensing.md`;
   qualified legal review pending. [S7]
2. Minimum supported OS versions and reference devices?
3. ~~Store review route for mobile executable plugins (Apple 4.7 bridge vs bundled native; Play
   rules)?~~ **Out-of-repo (2026-10-03)** — mobile distribution and its store gates are not this
   repository's concern; they belong to the separate mobile repository (future). [S5][S6]
4. ~~Which sandbox/worker approach wins the threat-model spike?~~ **Dropped (2026-10-03)** — the web
   mini-app/WASM sandbox option is removed; extension UI is native (Qt/QML panels from native
   extensions), see `docs/architecture.md` §10.
5. **Re-scoped (2026-10-03).** No fallback engine is planned and GES is the only planned engine; the
   remaining open item is the desktop playback/seek/timeline/composite/export gate itself. Mobile is
   out-of-repo (native adapters per `docs/decisions/engine-choice.md` §4).
