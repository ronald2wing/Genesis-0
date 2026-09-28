# Level 3 — Replace Core Behavior — Design

Status: proposed (design only, no source is modified by it)
Predecessor: Level 2 (`docs/proposals/extension-modification.md`, implemented).
This document designs how an extension replaces a **host controller** or hooks
a **hot path**, so it can change how the engine/app *behaves* — not just what
surface shows. It is a design for review; implementation is gated on the
security review in §5.

---

## 0. The goal and the hard constraint

Level 2 let an extension swap declarative/QML/asset content. Level 3 lets an
extension replace **core behavior**: the playback controller, the export
controller, the edit controller, or a resolution hot path (`build_timeline`,
seek/scrub, effect resolve).

The security posture is unchanged from Level 2 and is the boundary condition of
this whole design:

> **The signed marketplace is the security boundary; the Developer origin is
> the freedom boundary.**

A native extension that replaces `PlaybackController` *is* the application. So
Level 3 must be **capability-gated on a verified catalog signature**, never on
`Origin::Curated` alone. This is the single most important line in this
document: **override is declarative and safe; behavior replacement is code and
privileged.**

> **Audit correction (applied).** The original wording gated Level 3 on
> `Origin::Curated`. A trust-boundary audit found that `Origin::Curated` is a
> *locally self-applicable label*: the CLI accepts `--origin curated` for any
> `dir`/`git` install (`src/cli/Commands.cpp`), and `origins.json` is a plain
> user-writable file read back with no signature. `Origin` is a curation/consent
> label, not a cryptographic fact (see `src/extensions/Trust.h`). The privileged
> gate must therefore be a distinct capability minted only by a verified
> catalog-entry signature — §5 is updated accordingly.

---

## 1. Ground truth (verified against the tree)

| Fact | Location |
|---|---|
| Composition root constructs all controllers, concrete types | `src/app/AppShell.cpp:359-572` |
| Controllers held by concrete pointer | `src/app/AppShell.h:170-191` |
| QML context properties bound to concrete pointers | `AppShell.cpp:911-932` |
| Controllers are QObject-based, signal-wired at construction | `AppShell.cpp:389-900` |
| V1 controller set (settled, D5): Playback, Export, Edit, EffectParams | — |
| Trust origins + `may_load_native` / `may_override` | `src/extensions/Trust.cpp` |
| ABI: append-only, size-negotiated | `src/extensions/ExtensionApi.h`; `GENESIS_EXTENSION_API_VERSION` |
| Native loader, never unloaded | `src/extensions/NativeExtension.{h,cpp}` |
| Handler surface is JSON-RPC `api::dispatch`, C99 `call_json` | `src/api/Api.{h,cpp}` |

The 18 controllers are constructed as concrete Qt classes, wired by concrete
signal/slot, and exposed to QML by concrete context property. There is **no
interface seam** today: replacing one means every `AppShell` member, every
`connect(...)`, and every QML binding must go through an indirection.

---

## 2. The architecture in three parts

### Part A — the interface seam (`IPlaybackController`, …)

Each v1 controller gets an abstract interface:

```
class IPlaybackController /* : QObject-compatible */ {
public:
    virtual ~IPlaybackController() = default;
    // the methods QML and AppShell actually call, plus the signals
    // they actually connect to (Qt signals can be part of the interface
    // via the usual Q_DECLARE pattern, or the interface is pure-virtual and
    // the concrete class owns the signals — see §4 open question).
};
```

`AppShell` holds `std::unique_ptr<IPlaybackController>` (and the other three)
instead of the concrete pointer, and every `connect` and context-property
binding targets the interface. The concrete classes (`PlaybackController`,
…) implement the interface. This is a **mechanical, behavior-preserving
refactor** when no extension replaces anything: the app runs exactly as today.

The interface must expose **only what AppShell/QML/other controllers use** —
that set is discoverable by inventorying the concrete call sites (grep for
`controller_->`, `exporter_->`, `edit_->`, `effectParams_->` in `AppShell.cpp`
and the QML files). The v1 interface is that exact observed surface, no more.

### Part B — the factory + registration

A replacement provider registers a **factory** at scan/load time. Two options:

- **B1 — controller table at load.** `ExtensionHost`/`NativeExtension` collects
  a `controllers` contribution: the extension ships a symbol (e.g.
  `genesis_register_controllers`) that returns a table of
  `{ controller_kind, factory_fn, user_data, priority }`. The app asks the host
  "who provides Playback?" after the scan, and constructs the winner.
- **B2 — ABI struct growth.** Into the append-only `size`-negotiated struct the
  host already passes to `genesis_extension_entry`, add a
  `register_controller` function pointer the extension fills in. Lower-level,
  but consistent with the existing C99 ABI.

**Recommendation: B1** — a dedicated registration symbol parallel to
`genesis_extension_entry`, because the controller set is a *separate* concern
from the existing menu/panel/dialog contributions and deserves its own
discovery path rather than cramming into the size-negotiated struct.

### Part C — the trust gate (`Origin::Curated` only)

A `controllers` contribution is loadable **only** when the extension is
`Origin::Curated` **and** (see §5) the signed pipeline verified it. Developer
and User are refused outright; Builtin is the app's own code and does not go
through this extension path. This is enforced at scan time (the record is
`failed` with reason `controllers require Curated origin`, mirroring the
existing `may_override`/`may_load_native` gates) **and** at construction time
(defence in depth: `AppShell` refuses a factory from a non-Curated record).

---

## 3. The v1 controller set (settled, D5)

Playback, Export, Edit, EffectParams. Each maps to one factory slot:

| Slot | Replaces | QML surface it feeds |
|---|---|---|
| `playback` | `PlaybackController` | `controller` context property |
| `export` | `ExportController` | `exporter` context property |
| `edit` | `EditController` | `edit` context property |
| `effect_params` | `EffectParamsController` | `effectParams` context property |

The remaining 14 controllers are **out of scope for v1** and stay concrete.
Growing the set later = adding one interface + one slot at a time.

---

## 4. Open questions (resolve before implementation)

| # | Question | Notes / recommendation |
|---|---|---|
| Q1 | Qt signals on the interface vs. on the concrete class? | Signals must be `connect`ed by AppShell/QML. Qt interfaces with signals need the interface to be a QObject or an abstract QObject subclass. Recommendation: each interface inherits `QObject`, concrete class inherits both the interface and the real base. Verify against the existing `QObject`-multiple-inheritance pattern the Qt controllers already use (they may already single-inherit QObject — check). |
| Q2 | Construction timing: controllers are built once at startup, but extension libraries are `dlopen`'d during `ExtensionHost::scan` (before controllers exist). | The factory must be resolved *after* scan but *before* controller construction. `AppShell` already constructs `extensions_` (line 359) before `controller_` (line 389), so the ordering is compatible — the host's records are available by the time controllers are built. Confirm the scan completes before line 389. |
| Q3 | What if a Curated replacement crashes at construction? | The app must fall back to the builtin controller and log, never crash on a bad extension (matches the existing "a bad library is a `failed` record, not fatal" rule in `ExtensionHost`). |
| Q4 | Hot-path hooks (`build_timeline`, seek/scrub, resolve) — in v1 or later? | **Later.** v1 is controller replacement only; hot-path hooks are a finer-grained, higher-frequency seam that needs its own perf + safety analysis (a per-frame extension callback is a wholly different risk profile). Documented out of v1 scope. |

---

## 5. Security review checklist (gate to implementation)

This must be completed and signed off before any Level 3 code lands:

1. **[ ] Verified-capability enforcement** — a `controllers` contribution is
   refused unless the extension carries a verified catalog-entry signature
   (distinct from `Origin`). `Origin::Curated` is NOT sufficient: it is a
   locally self-applicable label (see the §0 audit correction), so gating on it
   would make the privilege self-mintable.
2. **[ ] Catalog signature is real, not the all-zeros placeholder** — as of the
   audit, `kCatalogPublicKey` (`src/extensions/Catalog.h`) is all zeros and
   `catalog_key_configured()` is false, so no entry can currently verify. Level
   3 cannot ship before a release key is minted and the verify path actually
   rejects an unverified/missing signature.
3. **[ ] No privilege escalation via override** — a controller replacement must
   not be reachable by chaining a Developer extension's `[[override]]` onto a
   signed extension's controller. The privileged gate keys on the
   *providing* extension's verified signature, independently of override
   resolution.
4. **[ ] Capability surface audit** — enumerate everything a replaced
   `PlaybackController` can reach (the `session_`, the `Editor`, the GL
   context, filesystem paths) and document that this is effectively
   full-app power; confirm that is the intended/signed contract.
5. **[ ] Crash isolation** — construction/`connect` failure falls back to
   builtin, logged, never fatal.
6. **[ ] ABI discipline** — the registration symbol is added append-only;
   `GENESIS_EXTENSION_API_VERSION` bump is deliberate and documented.

The audit is now complete (2026-10-10): the gateway as originally specified was
toothless, because `Origin::Curated` is mintable three ways — the CLI's
`--origin curated` flag (`Commands.cpp`), a hand-written `origin.json` entry
(`Origins.cpp`), and a catalog install passing the user-supplied origin through
(`Commands.cpp` → `Catalog.cpp`). None of these verifies a signature; the
catalog signing key is all zeros. The corrected gate (items 1-2) requires a
distinct, signature-verified capability, which does not exist yet — so Level 3
implementation is blocked until the catalog signing key and verify path are
real.

---

## 6. Stage breakdown (executable, after review sign-off)

- **Stage L3-1 — interface extraction** (mechanical, behavior-preserving).
  Introduce `IPlaybackController`/`IExportController`/`IEditController`/
  `IEffectParamsController`; repoint `AppShell` + QML. Full suite must stay
  100% green (this proves the refactor is behavior-neutral).
- **Stage L3-2 — factory + registration symbol** (B1). `genesis_register_controllers`
  discovered by `NativeExtension`; records carry a `controllers` set.
- **Stage L3-3 — trust gate** (Curated-only) + `AppShell` construction swap
  point with builtin fallback.
- **Stage L3-4 — tests** (hermetic: fake Curated records, factory returning a
  stub controller; assert construction, fallback on crash, refusal for
  Developer/User).
- **Stage L3-5 — docs** (`docs/extensions.md` §controllers; `AGENTS.md`).

---

## 7. Verification

- Behavioral-neutrality proof at L3-1: full `ctest` 100% green, zero behavioral
  delta (the whole point of the mechanical interface extraction).
- L3-2..4: new hermetic tests per stage; `format_check` + `qml_format_check`;
  smoke (`genesis-dev smoke`).
- Security review §5 items checked off before any L3-2+ merge.