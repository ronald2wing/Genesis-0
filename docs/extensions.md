# Extensions

Author guide for the Genesis-0 Extension SDK: the native, shared-library
Extensions introduced alongside the existing declarative Packs. It covers the
manifest, the C ABI, the capabilities an Extension can contribute, the host
services it can call, the build/install/consent workflow, and the security model
— stated honestly, including what is *not* protected.

The worked example is [`examples/hello-extension/`](../examples/hello-extension/).

---

## 1. What an Extension is

**Extension** is the umbrella term. There are two kinds:

- **Packs** — declarative data/GLSL effect bundles. The official set ships as
  the builtin data-only `genesis.packs` extension
  (`extensions/builtin/genesis.packs/packs/genesis.*`: a `effect.toml` manifest
  plus shader sources). The host
  compiles and runs the shader; a Pack contains no executable code of its own.
- **Native extensions** — shared libraries that contribute native code and UI
  (menus, QML panels, bundled packs). This is the public **Extension SDK**
  described here.

The word *plugin* is reserved for technical framework objects (Qt/GStreamer/GES
elements, a manifest `kind` field), never for this product surface.

A native extension is **not linked into the app**. It is built separately
against the SDK header, installed into an Extension store, and loaded at runtime
under the trust rules in §8.

The manifest `kind` field distinguishes the two: native extensions declare
`kind = "native"`; Packs declare `kind = "effect"`. Install detects the kind
from which manifest file is present (`extension.toml` vs `effect.toml`).

### Official builtins

Two builtins ship today, both under `extensions/builtin/` and both seeded into
the store on startup (see §6):

- **`genesis.packs`** — a data-only native extension (no `entry`). Its
  `extension.toml` lists the 83 official pack ids under its `packs/genesis.*`
  directories; the seeded `packs/` directory is the catalogue's official pack
  root (`Catalogue::source_root()` is the developer-build fallback).
- **`genesis.titles`** — the first tool converted to an extension: a data-only
  native extension with a single `[[panel]]` (`TitlesPanel.qml`). It replaces
  the old `TitleGalleryDialog` and reaches the host only through the
  `extensions.call` bridge (`template.list`, `template.fill`, `edit.apply`).
  Its panel imports `import GenesisApp 1.0` and reads the shared
  `extensions`/`controller`/`timelineModel` context properties, following the
  panel pattern in §3.

## 2. Anatomy

A native extension holds a manifest plus the contributions it names — a shared
library and/or QML panels and bundled Packs. A **data-only** extension omits
the library and contributes only panels/packs:

```
my-extension/
  extension.toml     required: identity + capabilities
  libmyext.so        the shared library, BESIDE the manifest (optional)
  MyPanel.qml        a panel, BESIDE the manifest
  packs/…            optional bundled Packs
```

Two path rules come from the manifest parser (`src/extensions/NativeManifest.cpp`):

- `extension.entry` and each panel's `qml` must be a **bare file name**. A path
  separator is rejected, so the library and the panel sit next to
  `extension.toml`, not in subdirectories.
- The shared library file name is platform-specific (`.so`/`.dylib`/`.dll`); set
  `entry` to the name your build produces.

Install copies the whole directory into the store under
`<store>/<id>/<version>/`, so keep everything the Extension needs there.

## 3. Manifest reference (`extension.toml`, format 2)

```toml
format = 2                          # optional; absent means 1; >2 is refused
                                    #   (format 1 still reads, without `requires`)

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
requires = ["author.dep"]           # optional (format 2); namespaced ids this
                                    #   extension depends on

[[menu]]                            # zero or more menu entries
path   = "Tools/Smart Blur"         # required, non-blank; the menu path
label  = "Smart Blur"               # required, non-blank; the entry label
action = "blur"                     # exactly one of action/call:
call   = "version"                  #   `action` -> invoke(action) in the library
                                    #   `call`   -> host API method via call_json

[[panel]]                           # zero or more QML panels
label = "Smart Blur"                # required, non-blank
qml   = "SmartBlurPanel.qml"        # required; bare file name beside the manifest
```

Validation (`validate_native`) enforces: a namespaced `id`; a non-blank `name`;
a safe `entry`; `entry` present whenever a menu `action` runs the library; `api
!= 0`; each menu has a `path`, a `label`, and **exactly one** of `action`/`call`;
each panel has a `label` and a safe `qml`; each bundled pack id and each
`requires` id is namespaced. A manifest that fails any check is refused, and
nothing is written to the store.

### Dependencies (`requires`)

Format 2 lets an extension name the ids it depends on. The host resolves the
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

### Bundled packs

`[extension].packs` lists Pack ids this Extension bundles (data only; the host
loads them by id). The manifest does not inline pack definitions.

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

`version`, `project.create`, `project.open`, `project.close`, `project.list`,
`project.get`, `project.document`, `project.save`, `project.setVideo`,
`edit.apply`, `edit.undo`, `edit.redo`, `media.probe`, `media.import`,
`catalogue.list`, `template.list`, `template.fill`, `template.instantiate`,
`template.save`, `export.run`, `export.cancel`, `export.status`,
`preview.frame`, `scopes.snapshot`, `edit.selection`, `preview.time`,
`media.list`, `gpu.status` (the store verbs `extensions.*` and `config.*` are
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
- Replies are JSON envelopes: `{"result": …}` on success, `{"error": {"code": …,
  "message": …}}` on failure. The example logs the raw `version` reply without
  parsing it.
- `log` sends a line to the host log; the host also logs its own refusals (bad
  ABI, id mismatch, missing consent, failed `invoke`) with reasons.

### Extension management methods

The store is managed through the same JSON-RPC surface (`extensions.*`); every
installed extension reports its `id`, `origin` (`builtin`/`curated`/`developer`/
`user`), its dependency edges (`requires`/`dependents`) and its load state
(`enabled`/`disabled`/`failed`, with a reason when not enabled).

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
   catalog URL (`extensions.catalog`) and installs a listed entry
   (`extensions.install` with the entry's `dir`/`git` source). Either way the
   dialog re-scans so the new row appears immediately. The app installs as
   origin **Developer** (the only origin a local directory can honestly claim),
   so the row lands consent-gated — see "Consent" below.
3. **Directory (CLI)** — `genesis-cli extensions install <dir>` copies a local
   directory holding `extension.toml` (native) or `effect.toml` (Pack) into the
   store.
4. **Git (CLI)** — `genesis-cli extensions install --git <url> [--ref <ref>]
   [--subdir <subdir>]` clones the repository and installs the tree at `subdir`
   (the root when omitted) at branch/tag/commit `ref` (the default branch when
   omitted).
5. **Catalog (CLI)** — `genesis-cli extensions install --catalog <url> <id>`
   fetches a marketplace catalog document and installs the entry named `id` from
   the source it records (§6, "Marketplace catalog").

The directory picker and the marketplace both install as origin **Developer**
(the marketplace passes the entry's `source` straight through
`extensions.install`); git installs remain CLI-only. The CLI forms for each
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
directory that holds `extension.toml` (or `effect.toml`) passed in. Pointing
install at an archive fails with "no extension.toml or effect.toml in …" — the
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
| `catalog <url>` | fetch and list a marketplace catalog |
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

The document is versioned JSON:

```json
{
  "format": 1,
  "entries": [
    { "id": "example.hello", "name": "Hello", "version": "1.2.0",
      "description": "A native hello extension",
      "source": { "type": "git", "url": "https://example.com/hello.git",
                  "ref": "v1.2.0", "subdir": "" },
      "sha256": "…" },
    { "id": "example.local", "name": "Local", "version": "1.0.0",
      "description": "Installed from a directory",
      "source": { "type": "dir", "url": "/srv/extensions/local" } }
  ]
}
```

- `format` is `1`; a higher format is refused, a lower one still reads.
- Each entry carries an `id` (a namespaced id, the same shape
  `NativeManifest` enforces), a `name`, a `version` (a string carried verbatim —
  the install reads the real version from the manifest, never this field), a
  `description`, and a `source` of `type` `dir` or `git` with a non-empty
  `url` (plus `ref`/`subdir` for `git`). `sha256` is optional.
- Parsing is all-or-nothing: any malformed entry (a blank/unnamespaced id, a
  missing name, a bad source) fails the whole catalog rather than half-listing
  it.

`genesis-cli extensions catalog <url>` fetches and lists the entries;
`genesis-cli extensions install --catalog <url> <id>` installs one, following
the entry's source through the same install path (a `git` entry clones, a `dir`
entry copies). The CLI fetches via the `curl` binary (`curl -fsSL <url>`), so
`curl` must be on `PATH`; the API's `extensions.catalog` method takes an
injected fetcher seam instead and reports `NotAvailable` when none is wired
(the same pattern as the config seam).

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

The profile is a versioned JSON document (`src/extensions/Config.{h,cpp}`):

```json
{
  "format": 1,
  "appVersion": "0.1.0",
  "settings": { "proxiesEnabled": true, "decodePreference": 1 },
  "extensions": [
    { "id": "example.hello", "version": 1, "origin": "developer",
      "source": { "type": "dir", "url": "/home/me/extensions/hello" } }
  ]
}
```

- `format` is the profile format version (currently `1`). A profile with a
  higher format is refused on import (a newer build wrote it); a lower one still
  reads.
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

Extension UI is limited to what the host exposes (menus, the panel area);
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
