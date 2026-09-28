# Extension Modification — Plan

Status: proposed
Scope: let extensions **modify existing functionality**, not just add to it.
Two levels, sequenced: first **override built-in surfaces** (Level 2), then
**replace core behavior** (Level 3). This plan is a design document; no source
is modified by it.

---

## 0. The requirement

> "Will the user be able to modify the existing software functionalities with
> extensions?" — answered: **yes**, in two stages.

Today an extension is **append-only**: it can add menus, panels, dialogs,
slots, and effect packs, and drive the host through the JSON-RPC verb surface
(`api::dispatch`). It **cannot** change what the app already does.

The goal is to close that gap:

- **Level 2 — override surfaces.** An extension replaces a *builtin*
  contribute-and-ship (a panel, dialog, effect pack, or title/LUT asset set)
  the app itself ships.
- **Level 3 — replace behavior.** An extension supplies a replacement for a
  *host controller* or hooks into a *hot path* (timeline build, seek/scrub,
  effect resolve), so it can change how the *engine/app* behaves, not just
  what surface shows.

---

## 1. Ground truth (verified against the tree)

| Fact | Location |
|---|---|
| Composition root constructs all controllers | `src/app/AppShell.cpp:313-572` |
| Controllers held by **concrete** pointer | `src/app/AppShell.h:170-191` |
| QML bound to concrete pointers | `AppShell.cpp:911-932` |
| Override entry model `<kind>:<id>` | `src/extensions/ExtensionManifest.h:87-97` |
| Override parse (`overrides` array) | `ExtensionManifest.cpp:165-201` |
| Override resolution (menus/panels/dialogs/slots) | `ExtensionHost.cpp:242-310` |
| Override trust gate `may_override` (Builtin/Curated only) | `Trust.h:69`; `ExtensionHost.cpp:472` |
| Builtin id/source/seed | `Seed.cpp:36-155` |
| Builtins ship: `genesis.packs`, `genesis.titles`, `genesis.scopes`, `genesis.export`, `genesis.captions`, `genesis.speech` (data-only panels/dialogs/effects) | `extensions/builtin/*/extension.toml` |
| Contributable kinds enum | `ExtensionManifest.h:76-84` (`CapabilityKind`) |

The builtin surfaces (`genesis.titles` panel, `genesis.export` dialog, the 123
`genesis.*` effect packs) are **themselves extensions** with `Origin::Builtin`.
So "override a builtin surface" and "override a contributed surface" are the
*same mechanism* — the difference today is that the override resolver and the
`may_override` gate were scoped narrow (Builtin/Curated declaring, and the
resolve targets being other *contributed* capabilities).

---

## 2. The two levels

### Level 2 — override built-in surfaces

**What:** an extension may declare an override that targets a *builtin*
contribution — replacing `genesis.titles`' panel, `genesis.export`'s dialog,
an individual `genesis.*` effect pack, or a title/LUT asset the app ships.

**Key realization:** this is mostly *broadening* what already exists — the
override resolver already walks `<kind>:<id>`; the builtins are already
extensions in the same scan. The work is:

1. **Target builtins in override resolution.** Ensure the resolver's
   `<kind>:<id>` lookup sees builtin contributions (they already do — builtins
   are scanned records) and that a Developer/Curated override can name a
   Builtin id (`genesis.titles`, `genesis.packs:genesis.glow`, …).
2. **Loosen the *declaring* origin gate.** Today only Builtin/Curated may
   declare overrides (`may_override`). For user-level "modify my own tool",
   we must decide whether **Developer** (consent-gated) may override a
   Builtin surface. Recommendation: allow **Developer overrides of
   Builtin/Curated contributions**, but keep the *override-of-code* rule
   separate (see Level 3 trust).
3. **Assets, not just UI.** Add override support for the *typed asset*
   contributions (`[[title]]`, `[[lut]]`, `[[effect]]`) — today override
   covers menus/panels/dialogs/slots only (`CapabilityKind`).

**Effort:** small-to-medium. No new interfaces; trust gate widens; asset-kinds
join the override table. Security: still QML/asset/declarative — a shader or
panel, not host code.

### Level 3 — replace core behavior

**What:** an extension replaces a host controller (`PlaybackController`,
`ExportController`, …) or hooks a hot path (`build_timeline`, seek/scrub,
effect resolve).

**What it costs (the honest part):**

1. **Interface seam.** Every replaceable controller must be abstracted behind
   an interface (`IPlaybackController`, `IExportController`, …) and held by
   interface pointer in `AppShell`, not the concrete type. That is ~18
   interfaces + a factory that an extension registers into.
2. **ABI growth.** `ExtensionApi.h` (append-only, size-negotiated) must add a
   controller-registration table (`GENESIS_EXTENSION_API_VERSION` bump).
3. **Trust redesign.** A native extension that replaces `PlaybackController`
   is *the entire application*. Consent is not a bound; the "shader cannot do
   arbitrary IO" promise collapses entirely for such an extension. This needs
   a new, much stricter trust tier ("privileged"), likely Curated-only and/or
   a signed-marketplace-only gate, plus an explicit capabilities grant.
4. **Wiring + lifetime.** QML context properties, signal wiring, and
   member-destruction ordering in `AppShell` assume concrete types; the swap
   point (controllers are built once at startup) must become re-entrant or
   extension-selected at boot.

**Effort:** large; multi-week; security-sensitive. Its own milestone.

---

## 3. Sequencing

Level 2 first, because it:
- delivers the visible "modify my tool" value immediately;
- forces the *override resolution* and *asset-kind* work that Level 3 reuses;
- touches only the declarative/QML/asset layer — no trust-boundary risk.

Level 3 is then designed on top, reusing the override vocabulary but adding the
interface seam + privileged trust tier.

---

## 4. Settled decisions

The security model: **the marketplace (Curated) is the security boundary; the
Developer origin is the freedom boundary.** A Developer can locally override
whatever they want, on their own machine, under recorded consent; anything that
runs as a *shipped, trusted* capability goes through the Curated/signed
pipeline. The line between "override" (swaps declarative/QML/asset content,
safe) and "run arbitrary host code" (Level 3, Curated/privileged only) is hard.

| # | Decision | Settled |
|---|---|---|
| D1 | Origins that may *declare* an override of a Builtin surface | Developer (consent-gated), Curated, Builtin may; User refused. |
| D2 | Origins that may be *targeted* by an override | Any non-User origin; builtin surfaces overridable by Developer/Curated. |
| D3 | Typed assets in override scope | `[[effect]]` (the 123 packs) joins now, first; title/LUT follow later. |
| D4 | Level 3 trust tier | "privileged" — Curated + signed-marketplace only, plus consent. |
| D5 | Level 3 controller set (v1) | Playback, Export, Edit, EffectParams; grow later. |

---

## 5. Stage breakdown (Level 2, executable)

**DONE — implemented and verified (119/119 tests, format_check +
qml_format_check pass).**

- **Stage L2-1 — widen override target scope.** Builtins were already valid
  override targets in `resolve_contributions` (they are in `candidate_ids`).
  Fixed the collision rule so an explicit override *wins* over trust rank
  (`ExtensionHost.cpp`), else a Developer could never beat a Builtin.
- **Stage L2-2 — widen declaring-origin gate.** `may_override` now returns true
  for Builtin/Curated/**Developer**; User still refused (`Trust.cpp:57-68`).
- **Stage L2-3 — add `[[effect]]` to override kinds.** `CapabilityKind::Effect`
  added; `override_kind` maps `"effect"`; `ExtensionRecord` carries `overrides`;
  `Catalogue::from_records` lets an overriding extension's effect pack win
  (`Catalogue.cpp:130-193`).
- **Stage L2-4 — docs.** `docs/extensions.md` §override + `AGENTS.md` still
  pending a wording touch-up (see follow-up below).

### Level 3 (design only, for now)

- **Stage L3-1 — interface seam** (planner: enumerate controller interfaces).
- **Stage L3-2 — privileged trust tier** (security review first).
- **Stage L3-3 — ABI registration table** (version bump).
- **Stage L3-4 — `AppShell` swap point.**

---

## 6. Verification

- L2: `ctest -R 'extension_host|extension_scan|trust|manager|extension_manifest'`
  plus full `ctest --test-dir build --output-on-failure`; `format_check` +
  `qml_format_check`; smoke (`genesis-dev smoke`).
- L3: (design review) interfaces enumerated, trust model written down and
  reviewed, no implementation until the security review signs off.