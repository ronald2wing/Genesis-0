# Extensions

Author guide for the Genesis-0 Extension SDK. **Extension** is the single
installable unit — one system, one manifest, one store, one discovery path, one
trust model for everything a user can add. The worked example is
[`examples/hello-extension/`](../examples/hello-extension/).

---

## 1. What an Extension is

**Extension** is the single installable unit. There is one system, one manifest,
one store, and one discovery path for everything a user can add. What an
extension *is* is what it ships:

- **A media pack** is an extension that ships declarative assets — effect
  bundles (GLSL), titles, LUTs, sounds, fonts — with no executable code. The
  official set ships as the builtin `genesis.packs` extension
  (`extensions/builtin/genesis.packs/packs/genesis.*`: `[[effect]]` blocks
  declaring shader sources). The host compiles and runs the shader; a data-only
  extension contains no executable code of its own.
- **A native extension** is an extension that additionally ships a shared
  library — native code and UI (menus, QML panels, dialogs) — possibly bundled
  with packs. This is the public **Extension SDK** described here.

There is no `kind` field separating the two. The one fact that makes an
extension "native" is the presence of an `entry` in its manifest: an extension
with `entry` loads a shared library (and is subject to the native trust rules in
§8); an extension without `entry` is data-only and loads freely when trusted.
What an extension *does* is communicated in its name and marketplace listing,
not encoded as a category.

The word *plugin* is reserved for technical framework objects (Qt/GStreamer/GES
elements), never for this product surface.

A native extension is **not linked into the app**. It is built separately
against the SDK header, installed into an Extension store, and loaded at runtime
under the trust rules in §8.

### Official builtins

Six builtins ship today, all under `extensions/builtin/` and all seeded into
the store on startup (see §6). Four contribute inspector panels and two
contribute dialogs:

- **`genesis.packs`** — a data-only native extension (no `entry`). Its
  `extension.toml` lists the 123 official pack ids under its `packs/genesis.*`
  directories; the seeded `packs/` directory is the catalogue's official pack
  root (`Catalogue::source_root()` is the developer-build fallback).
- **`genesis.titles`** — the first tool converted to an extension: a data-only
  native extension with a single `[[panel]]` (`TitlesPanel.qml`). It replaces
  the old `TitleGalleryDialog` and reaches the host only through the
  `extensions.call` bridge (`template.list`, `template.fill`, `edit.apply`).
  Its panel imports `import GenesisApp 1.0` and reads the shared
  `extensions`/`controller`/`timelineModel` context properties, following the
  panel pattern in §3.
- **`genesis.scopes`** — the scopes tool converted to a data-only native
  extension with a single `[[panel]]` (`ScopesPanel.qml`). It consumes the
  `scopes.snapshot` host method, polling it at ~10 Hz through `extensions.call`
  while visible (the SDK has no push channel; the old pane repainted on
  `frameChanged`). The reply's `hdr`/`scaleMax` follow the frame's colorimetry:
  HDR (PQ/HLG) is detected from the frame caps and the probe metadata (via
  `signal_from_caps`, `src/adapters/engine/ges/media/Colorimetry.h`) rather than
  a caller-injected default, falling back to SDR when the transfer metadata is
  absent. There is no `scopes.enable` bridge verb, so as its visibility
  changes the panel enables and disables the controller's off-thread sampler
  through the shared `scopes` context property instead.
- **`genesis.export`** — the export tool converted to a data-only native
  extension with a single `[[panel]]` (`ExportPanel.qml`), a port of the old
  export dialog using the `export.presets` / `export.run` / `export.status` /
  `export.cancel` host methods through the `extensions.call` bridge.
- **`genesis.captions`** — the captions tool converted to a data-only native
  extension with a single `[[dialog]]` (`CaptionsDialog.qml`), a port of the old
  caption overlay using the `ai.models` / `ai.download` / `ai.status` /
  `ai.run` / `ai.cancel` and `edit.selection` host methods through the
  `extensions.call` bridge.
- **`genesis.speech`** — the text-to-speech tool converted to a data-only native
  extension with a single `[[dialog]]` (`SpeechDialog.qml`), a port of the old
  speech overlay using the same AI methods through `extensions.call`, plus the
  `ai.models` voice list (`voices`) to feed its voice picker.

The two dialog builtins exercise the manifest's `[[dialog]]` capability — a
contributed dialog opened by the dialog overlay host — where the other four
contribute `[[panel]]` inspector tabs.

## 2. Anatomy

A native extension holds a manifest plus the contributions it names — a shared
library and/or QML panels/dialogs and bundled Packs. A **data-only** extension
omits the library and contributes only panels/dialogs/packs:

```
my-extension/
  extension.toml     required: identity + capabilities
  libmyext.so        the shared library, BESIDE the manifest (optional)
  MyPanel.qml        a panel, BESIDE the manifest
  MyDialog.qml       a dialog, BESIDE the manifest
  packs/…            optional bundled Packs
```

Two path rules come from the manifest parser (`src/extensions/NativeManifest.cpp`):

- `extension.entry` and each panel/dialog's `qml` must be a **bare file name**.
  A path separator is rejected, so the library and the QML sit next to
  `extension.toml`, not in subdirectories.
- The shared library file name is platform-specific (`.so`/`.dylib`/`.dll`); set
  `entry` to the name your build produces.

Install copies the whole directory into the store under
`<store>/<id>/<version>/`, so keep everything the Extension needs there.

## 3. Manifest reference (`extension.toml`)

There is exactly one manifest format, with no versioning and no `format` key:
a manifest is read as-is, and an unknown key (including an obsolete `format`)
is ignored.

```toml
[extension]                         # required table
id    = "author.name"               # required; namespaced: two lower-case
                                    #   segments (letters, digits, hyphens)
name  = "Human Name"                # required, non-blank
kind  = "native"                    # required; must be exactly "native"
version = 1                         # optional; whole number, defaults to 1
api   = 1                           # optional; SDK API version, defaults to 1,
                                    #   must not be 0
entry = "libmyext.so"               # optional; bare library file name beside
                                    #   the manifest. Required only when a menu
                                    #   `action` runs the library; absent for a
                                    #   data-only extension (packs/panels only)
packs = ["author.a", "author.b"]    # optional; bundled pack ids (namespaced)
requires = ["author.dep"]           # optional; namespaced ids this extension
                                    #   depends on
overrides = ["panel:author.other"]  # optional; capabilities this extension
                                    #   replaces: `<kind>:<id>`, kind one of
                                    #   `action`/`panel`/`dialog`. Only Builtin
                                    #   and Curated may declare one (see
                                    #   "Capability ids and overrides")

[[menu]]                            # zero or more menu entries
id     = "author.menu"              # optional; the capability id (namespaced,
                                    #   or the default `<ext-id>:<index>`)
path   = "Tools/Smart Blur"         # required, non-blank; the menu path
label  = "Smart Blur"               # required, non-blank; the entry label
action = "blur"                     # exactly one of action/call:
call   = "version"                  #   `action` -> invoke(action) in the library
                                    #   `call`   -> host API method via call_json

[[panel]]                           # zero or more QML panels
id    = "author.panel"              # optional; the capability id, as for `menu`
label = "Smart Blur"                # required, non-blank
qml   = "SmartBlurPanel.qml"        # required; bare file name beside the manifest

[[dialog]]                          # zero or more full-size dialogs
id        = "author.dialog"         # optional; the capability id, as for `menu`
label     = "Smart Blur…"           # required, non-blank
qml       = "SmartBlurDialog.qml"   # required; bare file name beside the manifest
menu_path = "Tools/Smart Blur"      # optional; the menu path the dialog's entry
                                    #   appears under (default: the Extensions menu)

[[slot]]                            # zero or more widgets embedded into a named
                                    #   host surface
point    = "clip.inspector"         # required; the host surface the widget
                                    #   embeds into (one of "clip.inspector" /
                                    #   "status.bar" / "settings.updates")
qml      = "InspectorSlot.qml"      # required; bare file name beside the manifest
priority = 50                       # optional; whole number 0..100, default 50;
                                    #   a lower value renders earlier
```

Validation (`validate_native`) enforces: a namespaced `id`; a non-blank `name`;
a safe `entry`; `entry` present whenever a menu `action` runs the library; `api
!= 0`; each menu has a `path`, a `label`, and **exactly one** of `action`/`call`;
each panel has a `label` and a safe `qml`; each dialog has a `label`, a safe
`qml`, and a non-blank `menu_path` when one is given; each slot has a `point`
naming a known host surface, a safe `qml`, and a `priority` between 0 and 100
when one is given; each capability `id` is namespaced or the default
`<ext-id>:<index>` form and is unique **within its kind** in the extension
(cross-kind reuse is allowed); each `overrides` entry is `<kind>:<id>` with a
known kind and a valid capability id; each bundled pack id and
each `requires` id is namespaced. A manifest that fails any check is refused,
and nothing is written to the store. The `overrides` syntax is validated
origin-free here; the *trust* gate on overrides is applied at install and scan
(§8), where the origin is known.

### Dependencies (`requires`)

An extension names the ids it depends on via `requires`. The host resolves the
graph when it loads the store (`src/extensions/ExtensionHost.cpp`):

- The scan Kahn-sorts the installed extensions and loads them in topological
  order, so a dependency is loaded (or refused) before its dependent.
- A **dependency cycle** fails every member with `dependency cycle: <path>`, and
  any tail waiting on the cycle fails with `requires <id>`.
- A `requires` id that is **not installed** fails the dependent with
  `requires <id>`. For a Pack there is no load step, so the id simply does not
  resolve.
- A dependency that is **disabled or failed** fails the dependent the same way,
  recording `requires <id>` as the reason, so the disappearance is explained
  rather than a silent no-load.

Disabling or removing an extension cascades down the reverse edges: every
transitive dependent is disabled/failed with `requires <parent>` naming the id
it directly depends on that went down (`Manager`, `src/extensions/Manager.cpp`).

### Menu capability

A menu entry is one command in the host's menu tree. Exactly one of:

- **`action = "<name>"`** — the host calls your library's
  `invoke("<name>", args)`. This is real functionality in your code.
- **`call = "<method>"`** — a *declarative* entry: the host routes `<method>` to
  its own API bridge (the same `call_json` surface of §5) and never calls your
  library. Use it to surface an API operation without shipping code for it.

The parser stores `call` as an opaque string; the host dispatches it as the
`call_json` method with an empty `{}` params object. (An inline-table form such
as `call = { method = "version", params = {} }` does **not** parse — the field
is a string.)

### Panel capability

Each `[[panel]]` names a QML file loaded into the host's extension panel area.
The QML is arbitrary, fully-trusted code run in the host's QML engine — see §8.

A panel lives in the installed-extension store, outside the app's own QML
directory, so it cannot resolve the host's components by file location. It
imports the host module instead:

    import QtQuick
    import GenesisApp 1.0

`GenesisApp` is the host's QML module, registered under the standard `/qt/qml`
resource prefix so it is importable at runtime from any panel. It exposes the
design system (`Theme`, `Panel`, `Button`, `InputField`) and the read-only
projections; the shared controllers (`extensions`, `controller`,
`timelineModel`, …) arrive as context properties on the panel's context. A
panel may stay with plain QtQuick types, but it should import the module
whenever it wants the host's look and controls.

A panel reaches the host API through the `extensions` context property's
`call(method, paramsJson)` invokable, which returns the same JSON-RPC reply
envelope as `call_json` (§5). **A panel runs with its extension's trust**: the
bridge does not re-check the panel's origin, so a panel may invoke any method
the extension it belongs to was granted — the panel and the extension it ships
with are one trust decision, not two.

Panels live in the inspector slot, roughly **100 logical px tall** in the default
shell, so a panel ported from a standalone dialog must compact: aim for about
two control rows, move secondary controls behind overlays (preset menus, an
option-disclosure drawer) rather than laying them out inline, and keep the
primary action pinned so it stays reachable at the slot's minimum height.
`extensions/builtin/genesis.export/ExportPanel.qml` is the precedent — the old
export dialog rendered as a compact panel.

### Dialog capability

Each `[[dialog]]` names a QML file the host opens as a **full-window modal
overlay**, the dialog counterpart to a panel. A panel sits in the ~100 px
inspector slot; a dialog is the surface an extension needs when a task wants the
whole window (the AI-tool conversions' original UX). Like a panel, the QML is
arbitrary, fully-trusted code run in the host's QML engine — see §8.

A dialog's menu entry appears under the host's Extensions menu (the menu tree
is flat, so `menu_path` is carried through to the projection but every entry
lands in the same place). Opening the entry routes through the controller's
`openDialog(id)`, which shows the first dialog that extension contributed.

The loaded dialog reaches two context properties through the root context:
`extensions` (the same `call(method, paramsJson)` bridge a panel uses, §5) and
`dialogBridge`, whose `close()` dismisses the overlay — the host clears the
Loader's source on close, which destroys the loaded component, so a closed
dialog leaks nothing. A dialog imports the host module exactly like a panel:

    import QtQuick
    import GenesisApp 1.0

The host wraps the load in a scrim and a blocking surface; a load failure shows
the id and the URL in the danger colour (and logs it), rather than a blank
window. `dialogBridge.close()` is the dialog's only lifecycle hook into the
host: everything else goes through `extensions.call`.

### Slot capability

Each `[[slot]]` embeds a QML widget into one of three named **host surfaces**,
rather than owning a whole inspector tab or window:

| `point` | Surface |
|---|---|
| `clip.inspector` | below the clip inspector's built-in controls |
| `status.bar` | the status bar, left of the GPU pill in the transport strip |
| `settings.updates` | below the update controls in the Settings dialog |

The host flattens every loaded extension's slots into a single `extensions.slots`
model — one `{id, point, url, priority}` map per entry — ordered by `priority`
ascending, ties broken by extension id. A lower `priority` therefore renders
earlier, and the ordering is deterministic regardless of install or load order.
The `url` is the `file://` URL of the contribution's QML, computed the same way
as a panel's or dialog's.

**Fail-closed on an unknown point.** The three names above are the only surfaces
the host renders; a slot that names anything else is a manifest refusal, not a
silent drop. A mistyped `point` must fail loudly at install rather than vanish a
contribution the author expected to see.

**Compact sections.** Each surface is a tight, fixed region — the inspector
section, the ~22 px status-bar pill row, and the Settings dialog's Updates column
— so a slot must compact the way a panel does: self-size to the loaded item's
implicit height/width (the host Loader does), keep the primary action reachable
at the slot's minimum height, and move secondary controls behind overlays rather
than laying them out inline. Like a panel or dialog, a slot's QML is arbitrary,
fully-trusted code run in the host's QML engine (§8), and it reaches the host API
through the `extensions` context property's `call(method, paramsJson)` bridge
(§5). A load failure renders a visible error surface naming the contribution's
id (in the danger colour) rather than a blank gap.

### Bundled packs

`[extension].packs` lists Pack ids this Extension bundles (data only; the host
loads them by id). The manifest does not inline pack definitions.

### Capability ids and `overrides`

A **capability** is a menu `action`, a panel, or a dialog a manifest
contributes. Each carries a stable **capability id**:

- the `id` key on the `[[menu]]`/`[[panel]]`/`[[dialog]]` table, or
- the default `<ext-id>:<index>`, where `index` is the 0-based position of the
  table within its own kind (menus, panels and dialogs each restart at 0).

An explicit id is either namespaced (`author.name`) or the default
`<ext-id>:<index>` form. Ids must be unique **within a kind** in one extension;
the same id may be reused across kinds (a menu and a panel can both be
`demo.tools:0`). Slots are excluded: they are ordered, non-exclusive
contributions, so a slot has no id and cannot be overridden.

`[extension].overrides` declares that this extension **replaces** another
extension's capability. Each entry is a string `<kind>:<id>`:

- `kind` is `action` (a menu entry), `panel` or `dialog` — the same three kinds
  an id can name.
- `id` is the target capability id (namespaced, or the default form, which
  carries its own colon). The parser splits on the **first** colon, so
  `panel:author.panel` and `action:author.menu:0` both parse.

Only the extension that **owns** a capability id may contribute it without an
override: id `X` is owned by extension `E` when `X == E` or `X` starts with
`E:` (candidate ids are namespaced, so the owner is unique). A contribution of
another extension's owned id is skipped and logged unless the contributor
declares the matching `<kind>:<id>` override. A free id (one no candidate owns)
may be contributed by anyone.

**Trust gate.** Overriding is a privilege of the trusted origins: only
**Builtin** and **Curated** may declare `overrides`. A Developer or User
manifest that declares one is refused **before anything is written**, with
`overrides are refused: only Builtin and Curated extensions may override`; the
same refusal is applied at scan time, so a hand-edited store cannot smuggle one
in. A Developer/User extension may still contribute free ids, and may still be
*overridden by* a trusted one.

**Resolution.** After the load phase, the host resolves each kind over the
extensions that loaded:

1. A contribution of an id owned by another extension is dropped and logged
   (`declares <kind> <id> owned by <owner> without an override; skipped`) unless
   its extension declared the matching override.
2. Among the surviving contributions to one id, the highest **trust rank** wins:
   Builtin > Curated > Developer > User. Equal rank is broken by the
   lexicographically smallest extension id, so the outcome is deterministic
   regardless of load order. Every loser is dropped and logged
   (`<kind> <id> loses to <winner>`).
3. An override that matches nothing — its extension does not contribute that
   `(kind, id)`, or no other candidate owns the id — is a warning, not a
   refusal: `<id> override <kind>:<id> matches nothing`.

Only the winners reach the loaded records (and so the menu/panel/dialog
projections). Resolution never rewrites the manifest on disk and never fails a
load: an unresolved override is a log line, and the rest of the extension loads.

**Fallback.** Resolution runs over the extensions that actually loaded. If the
winner is disabled, revoked, removed or failed, it contributes nothing and the
next-ranked surviving contributor of the same id wins; if none survives, the
capability is simply not surfaced. Because the owner set covers every installed
id (loaded or not), a disabled owner still owns its id: another extension can
claim it only by declaring an override, which takes effect as long as the
claimant loads. Turning the override back off (or removing its extension) makes
the original contribution reappear on the next scan — an override is data, so
disable/remove always has a deterministic fallback.

**Example.** A Curated extension contributes a panel with the default id
`cur.tools:0`; a Builtin replaces it:

```toml
# cur.tools/extension.toml
[[panel]]
label = "Tools"
qml   = "Tools.qml"          # id defaults to "cur.tools:0"

# bld.tools/extension.toml
overrides = ["panel:cur.tools:0"]
[[panel]]
id    = "cur.tools:0"        # must repeat the id it replaces
label = "Tools"
qml   = "MyTools.qml"
```

Both load; `bld.tools`'s panel is surfaced and `cur.tools`'s is dropped
(logged). Disable `bld.tools` and `cur.tools`'s panel returns. The namespaced
form is the alternative: `overrides = ["dialog:acme.picker"]` replaces the
dialog whose id is `acme.picker`.

## 4. The C ABI (`extensions/ExtensionApi.h`)

The ABI is pure C99: no C++ types, no exceptions, no RTTI. Include
`extensions/ExtensionApi.h` (the include root is `src/`).

### Versioning

- `GENESIS_EXTENSION_API_VERSION` is `1`; the manifest's `api` must match the
  host's, or the load is refused.
- Every struct carries a leading `uint32_t size`. The layout is **append-only**:
  a writer sets `size = sizeof(the struct it filled)`; a reader reads a field
  only when `offsetof(field) + sizeof(field) <= size`. New fields are appended,
  never inserted or reordered, so an older library and a newer host negotiate
  which fields exist.

### Entry point

The host resolves exactly one symbol, `GENESIS_EXTENSION_ENTRY`
(`"genesis_extension_entry"`), and calls it once after loading:

```c
int genesis_extension_entry(const GenesisHostV1* host,
                            GenesisExtensionV1* ext);
```

Fill `ext` and return `GENESIS_EXT_OK`, or a negative `GENESIS_EXT_ERR_*`
(`ABI`, `ID`, `ARG`, `FAILED`). `GENESIS_EXT_ERR_ABI` means the host's
`size`/`abi_version` is not recognized.

### The host table (`GenesisHostV1`)

| Field | Purpose |
|---|---|
| `size`, `abi_version` | struct size and `GENESIS_EXTENSION_API_VERSION` |
| `call_json(host, method, params, out, out_capacity)` | route a JSON request to the host API |
| `log(host, message)` | send a line to the host log |
| `reserved` | host-owned context; pass it back to the callbacks uninterpreted |

`call_json` returns `GENESIS_EXT_OK` with a NUL-terminated JSON reply in `out`
(up to `out_capacity` including the NUL), a **positive required size** when
`out` is too small (nothing written), or a negative `GENESIS_EXT_ERR_*`.

### The extension descriptor (`GenesisExtensionV1`)

| Field | Notes |
|---|---|
| `size` | `sizeof(GenesisExtensionV1)` |
| `id` | must equal the manifest's `id`; the loader cross-checks |
| `invoke(action, args, result)` | may be `NULL`; returns JSON via `*result` |
| `on_project_opened(path)` | may be `NULL`; notified after a project opens |
| `on_shutdown()` | may be `NULL`; notified before teardown |

`invoke` returns a JSON reply as a `char*` **allocated by the extension with
`malloc`**, which the host frees with `free`; `NULL` means no payload. The
host calls `invoke` for each menu entry whose `action` names a case.

### Lifetimes and threading — read this

- **Strings** handed to the host are UTF-8 and valid only for the duration of
  the call that passed them. A returned `*result` is the exception: it is
  `malloc`'d by the extension and `free`'d by the host.
- **Every host callback** (`call_json`, `log`) runs on the **loading thread
  (the host's main thread) only**, synchronously. Do not call one from another
  thread. Do not retain the `host` pointer past `entry` — copy what you need
  (the example stashes it in a static, which is fine because calls stay on the
  loading thread).
- **No unload.** A loaded library is never unloaded for API v1: it may have
  installed global state, `atexit` handlers, or threads that a `dlclose` would
  tear out from under. Only the dlsym-miss path closes a handle, because the
  entry never ran there.
- **Crash = host crash.** The library runs in the host process and has not been
  sandboxed or process-isolated. A segfault, a blocking call on the loading
  thread, or a C++ exception thrown across the boundary takes the host down.
  The host validates the ABI, id, and trust, but it cannot make your code safe.

## 5. Host services

`call_json` is the Extension's window into the same JSON-RPC surface the CLI and
the app use. Known methods (`src/api/Requests.cpp`):

`version`, `project.create`, `project.open`, `project.close`,
`project.get`, `project.document`, `project.save`, `project.setVideo`,
`edit.apply`, `edit.undo`, `edit.redo`, `media.probe`, `media.import`,
`catalogue.list`, `template.list`, `template.fill`,
`export.run`, `export.cancel`, `export.status`,
`export.presets`, `scopes.snapshot`, `edit.selection`,
`preview.time`,
`media.list`, `gpu.status`, `ui.paint`, `ai.models`, `ai.download`, `ai.status`,
`ai.run`, `ai.cancel` (the store verbs `extensions.*` and `config.*` are
described below and in §7). The app's `Services` wires all of these; the
headless CLI's `cli_services` wires only management + config + catalog, so the
rest are refused there as `NotAvailable`/`Failed`.

- `edit.apply` carries a project command; it is how an Extension mutates the
  project through the host (so undo/redo works). A command that does not decode
  is refused.
- `template.list` returns the in-house title-template library (one object per
  template: `name`, and `slots` of `{placeholder, style}` where `style` is the
  document-layer `TextStyle` JSON). `template.fill` fills one template's slots
  with caller text and returns the per-slot `{text, style}` pairs, each `style`
  already `content`-filled so a pair can be handed straight to an
  `edit.apply` `addTextClip` command.
- `export.presets` returns the named export targets (`{presets: [{name, width,
  height, rateNum, rateDen, fps, codec, quality}]}`), the same catalogue the
  builtin `genesis.export` panel
  (`extensions/builtin/genesis.export/ExportPanel.qml`) offers. `export.run`
  takes `{path, spec: {output,
  preset, width?, height?, rateNum?, rateDen?, codec?}}`; `spec.preset` is one
  of the `export.presets` names, and `spec.output` the file to write.
- `ui.paint` turns the shell's stroke-painting overlay on or off: takes
  `{active: bool, clipId?: string}` and returns `{"result": {}}`. `active`
  gates the overlay; `clipId` is optional because the shell keeps it synced to
  the selection, and it says which clip the overlay paints onto.
- Replies are JSON envelopes: `{"result": …}` on success, `{"error": {"code": …,
  "message": …}}` on failure. The example logs the raw `version` reply without
  parsing it.
- `log` sends a line to the host log; the host also logs its own refusals (bad
  ABI, id mismatch, missing consent, failed `invoke`) with reasons.

**AI flow.** The five `ai.*` verbs drive the four AI controllers (captions,
cutout, enhance, tts) behind one kind-based surface, where `kind` is `captions`,
`cutout`, `enhance` or `tts`. `ai.models` lists each kind's model(s) with their
install state and size (no network); `ai.download <kind>` starts installing the
kind's model(s) — the API call is the consent, so there is no interactive
prompt — and progress is polled through `ai.status`, which reports each kind's
`state` (`idle`/`downloading`/`running`/`done`/`failed`), `progress` (0..1) and
`error`. `ai.run <kind>` starts a run (captions take `clipId`, enhance takes
`clipId` plus `params.factor`, cutout takes `params.subject`, tts takes
`params.text` plus optional `voice`/`speed`);
it is refused as `NotAvailable` when the runtime or model is unavailable, so a
caller can `ai.download` first. `ai.cancel <kind>` stops a running or
downloading kind. The verbs are **app-bound in v1**: only the app owns the four
controllers, so the Qt-free CLI (whose `cli_services` never wires `ai`) refuses
all of them as `NotAvailable`; a Qt-free runner is a noted follow-up.

### Extension management methods

The store is managed through the same JSON-RPC surface (`extensions.*`); every
installed extension reports its `id`, `origin` (`builtin`/`curated`/`developer`/
`user`), its dependency edges (`requires`/`dependents`) and its load state
(`enabled`/`disabled`/`revoked`/`failed`, with a reason when not enabled).

| Method | Params | Effect |
|---|---|---|
| `extensions.list` | — | the installed extensions and their state |
| `extensions.install` | `source` (`dir`/`git`), optional `origin` | validate and install from a source; `origin` defaults to `developer` (consent-gated) |
| `extensions.remove` | `id` | tombstones and deletes a Curated/Developer id, disabling dependents; **refuses a Builtin** |
| `extensions.enable` | `id` | ungates a Developer id (grants consent) or clears a Builtin/Curated id's disabled flag |
| `extensions.disable` | `id` | revokes Developer consent or flags a Builtin/Curated id disabled; cascades to transitive dependents |
| `extensions.restore` | `id` | clears a Curated/Developer tombstone; **refuses a Builtin** |

A Builtin is removed/restored through the dedicated path the CLI and the app's
`ExtensionsController` use (§6), not through `extensions.remove`/`restore`.

## 6. Build, install, consent

### Build

Build against the SDK header as a standalone CMake project (do not add the
Extension to the host's CMake build). The example's
[`CMakeLists.txt`](../examples/hello-extension/CMakeLists.txt) is a starting
point: it compiles `hello.c` as a `SHARED` library and writes it beside the
manifest.

```sh
cmake -S . -B build -G Ninja && cmake --build build
```

### Install

Extensions reach the store over the same `<store>/<id>/<version>/` layout and
the same trust rules, from five install sources:

1. **Builtin** — the first-party extensions under `extensions/builtin/` are
   seeded on every start (§6, "Builtins and removal"). Nothing to do.
2. **In-app** — the Extensions dialog offers two entry points: **Install
   extension…** opens a folder picker, and the **marketplace** section fetches a
   catalog URL (`extensions.catalog`) and installs a listed entry through the
   controller's catalog-aware `installFromCatalog(url, id)`, which enforces the
   catalog's revocation list and persists it. Either way the
   dialog re-scans so the new row appears immediately. The app installs as
   origin **Developer** (the only origin a local directory can honestly claim),
   so the row lands consent-gated — see "Consent" below.
3. **Directory (CLI)** — `genesis-cli extensions install <dir>` copies a local
   directory holding `extension.toml` into the store.
4. **Git (CLI)** — `genesis-cli extensions install --git <url> [--ref <ref>]
   [--subdir <subdir>]` clones the repository and installs the tree at `subdir`
   (the root when omitted) at branch/tag/commit `ref` (the default branch when
   omitted).
5. **Catalog (CLI)** — `genesis-cli extensions install --catalog <url> <id>`
   fetches a marketplace catalog document and installs the entry named `id` from
   the source it records (§6, "Marketplace catalog").

The directory picker and the marketplace both install as origin **Developer**
(the marketplace routes through the catalog-aware `installFromCatalog`, so the
catalog's revocation list is enforced and persisted); git installs remain
CLI-only. The CLI forms for each
source:

```sh
genesis-cli extensions install --origin developer <extension-dir>
genesis-cli extensions install --origin developer --git <url> --ref <ref> --subdir <subdir>
genesis-cli extensions install --origin developer --catalog <url> <id>
```

The store defaults to `$GENESIS_EXTENSIONS_DIR`, else
`$XDG_DATA_HOME/genesis/extensions`, else `~/.local/share/genesis/extensions`;
pass `--store <dir>` to override. The default origin is `user`, which is
refused — a source never elevates trust, so pick `developer` (consent-gated)
or `curated` to actually install.

**The directory source is a directory, not an archive.** Install copies the
directory verbatim, so a `.zip`/`.tar.gz` must be **extracted first** and the
directory that holds `extension.toml` passed in. Pointing
install at an archive fails with "no extension.toml in …" — the
manifest is never found inside the archive.

**Git installs shell out to the `git` binary** (`git clone --depth 1
[--branch <ref>] <url> <temp-dir>`): the repository is cloned into a fresh
temporary directory, the manifest (and, for `subdir`, the subdirectory) is
validated against the store, and only then copied into the store atomically. The
clone is removed afterwards on every path. Git must be on `PATH`; the precise
reason is reported when it is not ("cannot start git"), when the process is
killed ("git clone was killed"), or when the clone exits non-zero ("git clone
failed (exit N): …", with the child's stderr). A `subdir` that escapes the
repository is refused as "the git subdir is not a safe path inside the
repository".

The `extensions` verb has eight subcommands, all over the same store:

| Verb | Effect |
|---|---|
| `list` | installed packs and native extensions, with `requires`/`dependents` edges |
| `install [--origin] [<dir> \| --git <url> \| --catalog <url>]` | validate and copy/clone an extension into the store |
| `catalog <url> [--store]` | fetch and list a marketplace catalog, persisting its `revoked` list |
| `trust [origin]` | print the policy (trusted / permissions) for an origin |
| `remove <id>` | tombstone and delete, disabling dependents |
| `restore <id>` | clear a tombstone and re-seed the builtins |
| `enable <id>` | ungrant/clear the disabled flag |
| `disable <id>` | revoke/gate, cascading to dependents |

Installing a native extension is "visible" only: the manifest is validated and
the directory is copied atomically into `<store>/<id>/<version>/`, but the
library is **never loaded at install time**. Consent gates the later load.

### Marketplace catalog

A **catalog** is a publisher's offer, the counterpart to a config profile
(§7): a profile is the machine's own installed set, a catalog is a list of
installable extensions with the source that would reproduce each. Install trusts
neither one more than the other — the same never-elevate-trust origin rules
apply. `src/extensions/Catalog.{h,cpp}` implements it.

The document is JSON; there is exactly one catalog format, so no version key
is carried or read:

```json
{
  "entries": [
    { "id": "example.hello", "name": "Hello", "version": "1.2.0",
      "description": "A native hello extension",
      "source": { "type": "git", "url": "https://example.com/hello.git",
                  "ref": "v1.2.0", "subdir": "" },
      "sha256": "…", "signature": "…" },
    { "id": "example.local", "name": "Local", "version": "1.0.0",
      "description": "Installed from a directory",
      "source": { "type": "dir", "url": "/srv/extensions/local" } }
  ],
  "revoked": [
    { "id": "example.old", "version": "1.0.0", "reason": "pulled for a security fix" }
  ]
}
```

- Each entry carries an `id` (a namespaced id, the same shape
  `NativeManifest` enforces), a `name`, a `version` (a string carried verbatim —
  the install reads the real version from the manifest, never this field), a
  `description`, and a `source` of `type` `dir` or `git` with a non-empty
  `url` (plus `ref`/`subdir` for `git`). `sha256` and `signature` are optional
  strings, both enforced by the install path — see "Integrity" below.
- Parsing is all-or-nothing: any malformed entry (a blank/unnamespaced id, a
  missing name, a bad source, a non-string `signature`) fails the whole catalog
  rather than half-listing it. A malformed `revoked` entry fails it the same way.
- The optional `revoked` array names installed extensions the publisher wants
  taken offline; each entry is an `id` plus an optional `version` (absent = every
  version) and an optional `reason`. See "Revocation" below.

`genesis-cli extensions catalog <url>` fetches and lists the entries;
`genesis-cli extensions install --catalog <url> <id>` installs one, following
the entry's source through the same install path (a `git` entry clones, a `dir`
entry copies). The CLI fetches via the `curl` binary (`curl -fsSL <url>`), so
`curl` must be on `PATH`; the API's `extensions.catalog` method takes an
injected fetcher seam instead and reports `NotAvailable` when none is wired
(the same pattern as the config seam).

### Integrity

A catalog entry carries two optional integrity fields, both enforced by the
install path (`install_from_catalog`), not by the list path:

- **`sha256`** — the content digest the source must match before install. The
  convention is a deterministic **tree digest** over the materialized source
  (below), spelled as 64 lowercase hex digits. A declared digest that is not 64
  lowercase hex is refused before any source work; a computed digest that
  differs from the declared one refuses the install before anything is copied to
  the store and (for a `git` source) removes the temporary clone.
- **`signature`** — the publisher's Ed25519 signature over the entry's
  **canonical signed payload**, the string `id + "\n" + sha256` (the id, a
  newline, then the digest). It binds the identity to the content, and because
  it covers only the digest it is checked before the source is fetched — the
  digest check then pins the actual bytes (the updater uses the same
  signature-over-digest model, `docs/decisions/auto-update.md` §4). A signature
  is **fail-closed**: an entry that declares one is refused whenever it cannot
  be verified, never installed unsigned (see "Signing status" below).

#### Tree digest

`tree_sha256` (`src/extensions/SourceInstall.{h,cpp}`) is the digest convention
for the two source types a catalog can express. It is the SHA-256 of a byte
stream built from the tree's **regular files** only:

1. Only regular files contribute bytes. A directory is implied by the paths of
   the files under it; symlinks and other non-regular entries are ignored (they
   are not content).
2. Each file contributes its path relative to the tree root (UTF-8, `/` as the
   separator), a single NUL byte, then its raw contents.
3. Files are fed in byte-wise lexicographic order of those relative paths, so
   the digest is independent of directory enumeration order.

The digest of an empty tree is the SHA-256 of the empty byte string. A tree that
cannot be read (not a directory, or an unreadable file) yields no digest, and a
verifying install refuses rather than guessing.

- **`dir`** — the digest covers the directory itself.
- **`git`** — the digest covers the checked-out tree at the resolved `subdir`
  (the repository root when `subdir` is empty), i.e. exactly what is installed.

**Archive/URL sources.** A catalog cannot express an archive/URL source today —
`source.type` is only `dir` or `git`. The convention for a future archive/URL
source is the digest of the **fetched bytes** (raw, before any extraction), the
same as the updater's package digest; it is documented here so the field's
meaning is unambiguous when such a source type is added, but nothing installs
from one yet.

#### Signing status

`signature` is parsed and enforced fail-closed. The reusable Ed25519 verifier is
`src/workspace/update/Signature.h` (`verify_ed25519`), which resolves to
`Unavailable` in the default build (`GENESIS_UPDATER` off) and to a real verify
when the updater is compiled with OpenSSL. The install path:

- refuses a signed entry with no `sha256` (nothing to bind the signature to);
- refuses a signed entry when crypto is not compiled in (`Unavailable`);
- otherwise verifies against the embedded catalog key `kCatalogPublicKey`
  (`src/extensions/Catalog.h`), refusing a signature that does not verify.

**The key is pending.** `kCatalogPublicKey` is all zeros until a catalog signing
key is minted, so even a crypto-enabled build refuses every signed entry ("no
catalog signing key is configured"). Which key signs catalog entries, where the
public half ships, and how it is rotated is documented in
[`tools/release-signing/`](../tools/release-signing/README.md) — the
key-generation/signing tooling for both the catalog key and the (separate)
release key in `src/workspace/update/Signature.h`, which are distinct trust
domains and never share a keypair. Until a key is minted a `signature` is a
publisher's promise that cannot yet verify, and the install refuses signed
entries rather than treating them as unsigned.

### Revocation

A catalog's `revoked` list is the publisher's way to take an installed
extension offline. It is **advisory data**, not a security boundary: a
revocation forces an extension off so the user can see why and remove it, it
never silently uninstalls, and it is only as trustworthy as the catalog that
declared it (a signed catalog bounds that trust; an unsigned catalog's list is
only as trustworthy as the catalog itself).

The mechanics (`src/extensions/RevokedList.{h,cpp}`):

- **Persistence.** The list is reconciled into `<store>/revoked.json` — the
  catalog is authoritative for its own revocations, so the store's list is
  **replaced** outright on each catalog install (`replace_revocations`): an id
  the catalog no longer revokes is cleared, and a later catalog that drops the
  entry re-enables it. The write stages to a temp file and renames, so the list
  is never observed half-written; an absent or corrupt file reads as nothing
  revoked.
- **Install refusal.** `install_from_catalog` refuses an entry whose id the
  catalog revokes (a version-less revocation, or one naming the entry's
  `version`), before any source work, carrying the publisher's reason.
- **Load refusal.** `ExtensionHost::scan` checks the store's list against each
  id's newest installed version and collects a match as `Revoked` (visible, not
  loaded, files kept so the user can remove it), overriding an otherwise-enabled
  or disabled id regardless of origin. The record carries the publisher's reason,
  or the bare `revoked` when none was given.
- **Re-enable refusal.** `Manager::enable` returns false for a revoked id — a
  revocation is a forced-off the user cannot toggle away, only clear (by removing
  the extension or by re-syncing a catalog that no longer revokes it).
- **Surfacing.** `list_extensions` orders revoked records between disabled and
  failed; the app's extensions list and the `extensions.list` reply spell the
  state as `"revoked"`.

A revocation is best-effort and local: the CLI `catalog` list verb persists the
catalog's `revoked` list into the store (pass `--store` to name it; a failed
write is reported as a `note:` but does not fail the list), so merely listing a
catalog forces an already-installed revoked id off on the next scan. The API's
`extensions.catalog` seam has no store handle, so it still reports the catalog's
`revoked` list without persisting it — the reconcile there happens on install
(via `installFromCatalog`), not on list.

### Publishing a catalog (and a default feed)

A publisher ships a catalog document and points users at its URL. A worked,
parseable sample is
[`extensions/catalog.example.json`](../extensions/catalog.example.json): three
entries with namespaced `example.*` ids — one `git` source with `ref`/`subdir`,
one bare `git` source, and one `dir` source. The `sha256` and `signature`
fields in the sample are **placeholders** (all-zeros hex) and must be replaced
with real values for a published catalog; the format has no comments, so the
explanations live here rather than in the file. An entry whose `sha256` is
still all zeros will not install (the digest will not match a real source), and
a `signature` is refused fail-closed until a key exists (see "Integrity" above),
so the placeholders are illustrative, never installable.

The catalog is the document of "Marketplace catalog" above. To publish:

1. Write a document listing each installable extension's `id`,
   `name`, `version`, `description`, `source`, and (optionally) `sha256` and
   `signature`.
2. Host it at a stable URL and pass that URL to the app's marketplace field or
   to `genesis-cli extensions catalog <url>`.

**The default feed.** There is **no hosted feed yet**: the app's default
catalog URL is empty (`AppSettings::kDefaultCatalogUrl`), so the marketplace
starts with a blank field and the user pastes a URL. To override it:

- **Per user** — type a URL into the marketplace field (or the CLI's `--catalog`
  argument). The app persists the field to `<config>/catalog.json` (the same
  config-dir pattern as the update feed), so it survives a restart. Clearing
  the field (fetching with it empty is refused) leaves the setting unset.
- **For a release** — set `AppSettings::kDefaultCatalogUrl` to the hosted feed
  URL when one is published; the empty constant documents the current
  "no feed yet" state.

**Testing with a local file.** The marketplace accepts a `file://` URL, so a
catalog can be exercised without hosting it:

```sh
genesis-cli extensions catalog file:///home/me/extensions/catalog.example.json
```

(Use the file's absolute path after `file://`; the CLI's curl fetch and the
app's fetcher both treat `file://` as a plain local read.) A `dir`-source
entry in a local catalog points at a directory on the testing machine, so the
whole install path is testable end-to-end against the sample.

**Signing is pending.** `signature` is parsed and enforced fail-closed, but no
catalog signing key exists yet, so a signed entry is refused in every build
until one is minted (see "Integrity" → "Signing status" above). The canonical
signed payload is already fixed (`id + "\n" + sha256`), so publishers can sign
entries today against a key they intend to ship later; the key/distribution
story itself is out of scope for now.

### Builtins and removal

The first-party extensions under `extensions/builtin/` are seeded into the
store on every start (`src/extensions/Seed.cpp`): a manifest-validated builtin
whose id is neither installed nor on the `removed.json` tombstone list is copied
in and recorded as origin `builtin`. The seed is idempotent and skips a
tombstoned id **forever** — a builtin the user removed never silently
reappears.

A builtin **is removable**: `genesis-cli extensions remove <id>` (and the app's
Extensions dialog) writes the id to `<store>/removed.json` and deletes the
installed copy; its dependents are disabled with `requires <id>`. Restore runs
`genesis-cli extensions restore <id>`, which clears the tombstone and re-seeds
the builtins, reinstalling the id. Reinstalling the same id through
`extensions install` also clears the tombstone (`src/extensions/Install.cpp`).

The management API (`extensions.remove`/`extensions.restore`) **refuses a
Builtin** — the API treats builtins as seed-managed. The CLI and the app's
`ExtensionsController` take the dedicated tombstone path above instead.

### Consent (Developer origin)

An unsigned, local-only Developer extension installs but does not load until the
user records consent for its id. Consent persists in the store at
`developer-consent.json`:

```json
{ "ids": ["example.hello"] }
```

An absent or unreadable file means nothing is consented (not an error). The app
prompts for consent in its Extensions panel: a Developer install appears as a
failed row whose reason names the missing consent, and its **Consent** toggle
grants it (the row then loads). There is no separate `consent` verb: for a
Developer id, `genesis-cli extensions enable <id>` records consent and
`disable <id>` revokes it (`Manager::enable`/`disable`), or the app writes the
   file. Builtin and Curated extensions never consult consent; they load freely.

## 7. Config profiles

A **config profile** is a portable snapshot of a machine's setup that a user can
carry between installs: the settings that are meaningful across machines, plus
the installed extensions with enough provenance (origin + install source) to
reproduce the install — or to decline it when it cannot be reproduced safely.

The profile is a JSON document (`src/extensions/Config.{h,cpp}`); there is
exactly one profile format, so no version key is carried or read:

```json
{
  "appVersion": "0.1.0",
  "settings": { "proxiesEnabled": true, "decodePreference": 1 },
  "extensions": [
    { "id": "example.hello", "version": 1, "origin": "developer",
      "source": { "type": "dir", "url": "/home/me/extensions/hello" } }
  ]
}
```

- `appVersion` is the exporting build's version, recorded so an import can
  surface a version mismatch.
- `settings` holds the portable subset (below). Every field is optional: a value
  is exported only when the host's settings seam reports it, and on import an
  absent field is left untouched.
- `extensions` lists the installed ids. Each entry carries the id's **highest
  installed version**, its recorded **origin** (`builtin`/`curated`/`developer`/
  `user`; `developer` when none was recorded), and its **source** — the install
  record from `<store>/sources.json` (`type` of `dir`/`git`/`builtin` plus a
  `url` for `dir` and `git`; a `git` source also carries `ref` and `subdir` when
  they are non-empty). An id with no recorded source exports `source: null`.

### Settings carried

The portable subset covers the settings a user re-picks on a fresh machine;
machine-specific paths are deliberately excluded:

| Setting | Field | Notes |
|---|---|---|
| Proxies enabled | `proxiesEnabled` | the render proxy/reverse cache toggle |
| Decode preference | `decodePreference` | `0` auto, `1` hardware, `2` software |
| Scopes interval | `scopesIntervalMs` | waveform/scopes sampling period |
| Update feed URL | `feedUrl` | the self-update feed |

The proxy/cache **root** is not carried — it is tied to the machine, not the
user. The getter/setter seam (`ConfigSeam` in `src/extensions/Config.h`) is
injected: the app wires it over `AppSettings`/`UpdateController`; the headless
CLI wires nothing, so every setting is omitted on export and reported skipped on
import.

### Export / import

**CLI** (over the default store or `--store <dir>`):

```sh
genesis-cli config export profile.json
genesis-cli config import profile.json                    # settings + extensions
genesis-cli config import profile.json --no-install       # settings only
```

**API** — `config.export` returns the profile document inline; `config.import`
takes the profile in `params.profile` and returns one line per setting
(applied/skipped) plus one entry per extension (`installed`/`skipped`/`failed`
with a reason). Both are refused as `NotAvailable` when the host wired no config
seam.

### Import trust rules

Import never elevates trust. For each profile extension, in order:

- **Already installed** (any version) → `skipped` ("already installed").
- **No source recorded** (`source: null`) → `skipped` (nothing to reproduce).
- **`builtin` source** → `skipped` (a builtin re-seeds on startup).
- **`dir`/`git` source from a `developer` origin** → installed through
  `install_from_source`, consent-gated as usual. A `dir` source installs only
  when the recorded path still exists (otherwise `failed`, "path missing"); a
  `git` source clones and installs, and `failed` carries the clone/install
  reason when the repository is unreachable or the install is refused.
- **`dir`/`git` source from a `curated`/`builtin` origin** → `skipped` ("trusted
  origin without a trusted source") — a bare path or URL must not re-trust a
  curated/builtin id. These are reported, never force-installed.
- **`user` origin** → `failed` (not installable).

A `developer` re-install records the same `dir`/`git` source, so a later export
stays reproducible, and stays **visible** and consent-gated (see "Consent"
above): importing the profile copies the extension in but does not grant
consent.

## 8. Security model

Trust follows from origin (`src/extensions/Trust.h`). Four origins:

| Origin | Install | Load | Notes |
|---|---|---|---|
| **Builtin** | yes | yes | first-party code in the host binary |
| **Curated** | yes | yes | from the signed marketplace pipeline (R7 review) |
| **Developer** | yes (visible) | only with recorded consent | unsigned, local-only |
| **User** | no | no | no signature, no review, no consent |

For Packs the gate differs: Builtin and Curated install; Developer and User are
refused, because a Pack has no load step at which consent could gate it.

**Overrides are a further gate.** Declaring an `overrides` entry is reserved to
the trusted origins: only **Builtin** and **Curated** may replace another
extension's menu action, panel or dialog; Developer and User are refused at
install and at scan (`may_override`, `src/extensions/Trust.h`). Overriding is
resolution, not code execution — but it lets a trusted extension change what
another extension's surface does, so it is gated like a capability rather than
granted with the origin that can merely *be* overridden. See "Capability ids and
`overrides`" in §3.

**There is no sandbox.** Native extension UI is native Qt/QML and native code
runs in the host process with the user's privileges; the shared QML engine is
not a sandbox. Loading an Extension is a full-trust decision:

- A Developer extension is unsigned. Consent means "load this id", not "this
  code is safe". Read the source before granting it.
- A Curated extension has passed the marketplace's review, but a signature is
  not a proof of safety and the host offers no C++/Qt ABI-stability promise:
  Extensions declare the ABI/toolchain they were built with.
- There is no unload, so a misbehaving Extension requires a restart to get rid
  of; arbitrary hot-swap is not promised.
- A crash in the library is a crash of the host.

Extension UI is limited to what the host exposes (menus, panels, dialogs);
there is no web mini-app or WASM runtime and none is planned.

## 9. Debugging

- The host log is the primary surface: it carries host refusals (with reasons)
  and everything the extension sends through `log`.
- A manifest refusal prints every problem at once with the offending key and
  line — fix the manifest and reinstall.
- Because the library runs in-process, ordinary native debugging of the host
  applies; a release build may not carry the Extension's symbols.

## 10. Packaging

An Extension is a directory: manifest + library + QML + any bundled packs.
Install copies that directory verbatim into `<store>/<id>/<version>/`, so ship
exactly what the manifest names, beside the manifest. A new version installs
alongside the old; reinstalling the same id+version overwrites deterministically.
For distribution outside the marketplace, mark the origin **Developer** and
expect a per-id consent prompt on every machine.
