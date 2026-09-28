# AGENTS.md

Repo-specific instructions for coding agents. Universal rules (commit
discipline, style) live in the global AGENTS.md and are not repeated here.

## Orientation (read this first)

- **`docs/README.md`** is the orientation index; **`docs/architecture.md`** is the
  system map (feature surfaces, debug loop). Read them before the code.
- **`build/bin/genesis-dev features`** lists every feature surface (API verbs, CLI
  verbs, command verbs, QML surfaces, controllers), parsed live from the tree so
  it cannot drift. Use it to find what the product does.
- **`build/bin/genesis-dev where <keyword>`** is the debug loop: a symptom word
  maps to matching features, the ctest suites that pin them, the smoke surface
  that exercises them, and the command to reproduce.
- **`build/bin/genesis-dev suites <file>`** maps a changed file to the suites
  that compile or link it, so you run the right `ctest -R` subset.

## Stack and layout

- C++23 cross-platform video editor. CMake >=3.28 + Ninja.
- Include root is `src/`, so includes look like `#include "project/Project.h"`.
- Targets:
  - `genesis_host` — engine-free, Qt-free core: `src/core`, `project`, `render`,
    `effects`, `extensions`, `interchange`, `jobs`, `api`, `workspace`, plus the
    engine-free half of `src/ai` (`ModelManifest`, `ModelStore`, `Sha256`,
    `vision/MaskStore`).
  - `genesis_ai` — AI provider library; engine-free and Qt-free, links
    `genesis_host`, always built. `src/ai/{captions,vision,tts}` providers report
    `disabled` when their runtime is not compiled in.
  - `genesis_engine_ges` — the GES adapter library; the only *library* that links
    GStreamer/GES. Subdirs `ges/{session,effects,media,scopes,export}`, where
    `ges/export/` holds `Export.cpp`, `Encoder.*`, `Hardware.*` (the bounded
    hardware probe, projected to QML as `gpu`) and `DecodePolicy.*`.
  - `genesis_app_core` — static library holding the app's sources; the thin
    `genesis_app` executable links it. Add app sources to the core target, not to
    the executable.
  - `genesis_app` — the only Qt + GES target; needs Qt6 >=6.5.
  - `genesis_cli` — host CLI; Qt-free, links the engine adapter for `render`.
    `src/cli/` holds `SocketServe` (Unix socket), `GrpcServe` (gRPC, gated),
    `JsonRpc` (shared codec) and `Commands` (the verbs).
  - tests, `perf_tests`.
- Seam headers app/host talk to: `src/adapters/engine/{EngineSession,Export,ScopeTypes}.h`.
- `src/app/AppShell` is the composition root (`GenesisApp.cpp` is the thin entry).
  Controllers are one class per file under `src/app/controllers/`, QML
  projections under `src/app/model/`, QML is a flat `src/app/qml/`, helpers under
  `src/app/{ai,net,support,render}/`. The root holds only `GenesisApp.cpp`,
  `AppShell.{h,cpp}`, `Version.h`.
- `src/workspace/` is the app-service layer (Projects/Templates/Logs/update), NOT
  the engine host core.
- `genesis::project::Editor` owns the project and swaps it in place on load —
  app controllers must hold the `Editor*`, not pointers into the project.

## Interchange (OTIO / CMX-3600 / FCPXML)

- Adapters live in `src/interchange/` (`Otio`, `Cmx3600`, `FcpXml`, plus
  `ProjectModel.h` for the shared `active_timeline`/`media_by_id`/`settled`
  helpers). They read and write the typed model, never hand-copied JSON keys.
- Host verbs: `interchange.export {path, format}` (`"otio"`, `"edl"`,
  `"fcpxml"`; an empty format is refused) and `interchange.import {path}`, which
  picks the reader from the **content** (`<` FCPXML, `{` OTIO, anything else
  CMX-3600). Both are handled in `dispatch`, so the CLI can use them headlessly.
- **Format logic belongs in C++, not QML.** The builtin `genesis.interchange`
  dialog only picks a path and calls the verbs. Putting a converter in QML
  duplicates the document schema and cannot be used headlessly.
- A Genesis export is authoritative from the **spine**: OTIO per-clip
  `metadata.genesis.clip` carries only what OTIO cannot express, and import
  starts from the recorded clip then overwrites timing/track/name/media from the
  file. An external retime must win over our snapshot.
- Import builds in the typed model and then passes through
  `from_document(to_document(...))`, so an imported file is held to the same
  invariants as a document read.

## Extension system

- The single installable unit is **extension**. One manifest, one store, one
  discovery path, one trust model for everything a user adds. A **media pack** is
  an extension shipping declarative assets (effect bundles, titles, LUTs, sounds,
  fonts) with no executable code; a **native extension** additionally ships a
  shared library (`entry`). There is no `kind` field — the presence of `entry` is
  the one signal for "carries native code". Use "extension" for this product
  surface; reserve "plugin" for technical framework objects (Qt/GStreamer/GES
  elements). Never call this system a "module" or "mini-app".
- Layout under `src/extensions/` (all in `genesis_host`, Qt-free/engine-free):
  `ExtensionApi.h` (C99 ABI, append-only `size`-negotiated structs),
  `ExtensionManifest` (parses `extension.toml`; `entry`/panel `qml` must be bare
  file names), `NativeExtension` (`dlopen`; never unloaded in v1),
  `ExtensionHost` (scan, highest version per id, gates), `Install`, `Trust`
  (`Origin{Builtin,Curated,Developer,User}`), `Consent`, `DisabledList`,
  `RemovedList`, `Origins`, `Seed`, `Manager`, `Sources`/`SourceInstall`,
  `Catalog`, `Config` (portable profile).
- **Store**: `<AppDataLocation>/extensions` (`~/.local/share/genesis-app/extensions`
  on Linux), laid out as `<id>/<version>/`. Inspect it when a builtin looks stale.
- **Seed upgrades builtins by version.** `Seed` installs a bundled extension only
  when its `version` is newer than anything installed; equal or newer stays put,
  and a tombstoned id stays gone. Editing a builtin's QML/assets therefore needs
  a `version` bump in its `extension.toml`, or every existing install keeps the
  old copy.
- Builtins under `extensions/builtin/` (copied beside the binary at build time):
  `genesis.packs` (data: 123 `[[effect]]` entries; 118 ship a GLSL body, the
  rest are audio/CPU packs), `genesis.titles`, `genesis.scopes`,
  `genesis.export`, `genesis.cutout`, `genesis.enhance` (panels),
  `genesis.captions`, `genesis.speech`, `genesis.interchange` (dialogs).
- Manifest is `extension.toml`, exactly one format. `[extension].requires =
  ["author.dep", ...]` lists namespaced ids; the scan Kahn-sorts and loads in
  topological order (a cycle fails its members with `dependency cycle: …`, a
  missing/disabled/failed dependency fails the dependent with `requires <id>`).
  Disabling or removing an extension cascades to its transitive dependents.
- A removed builtin is tombstoned in the store's `removed.json` and never
  silently re-added; `extensions restore <id>` (or reinstalling) clears it.
  `extensions.remove` refuses a Builtin over the API; the CLI and app dialog
  remove them directly.
- **Bridge.** Panels and dialogs reach the host through the `extensions` context
  property's `call(method, paramsJson)` invokable, which routes to the same
  `api::dispatch` JSON-RPC surface as `call_json` and returns the reply envelope.
  Panels import `import GenesisApp 1.0` because they live outside the app's QML
  directory.
- **Served-method split.** The app's `Services` (`AppShell.cpp`) wires extension
  management (`extensions.*`), `config.*`, `extensions.catalog`, `media.probe`,
  `catalogue.list`, `export.*`, `scopes.snapshot`, `edit.selection`,
  `preview.time`, `media.list`, `gpu.status`, `ui.paint` and the AI verbs.
  `interchange.*` is handled directly in `dispatch`, so it works in the CLI too.
  The CLI (`cli_services`) wires management + config + catalog + `media.probe` +
  `catalogue.list`; the rest are refused there as `NotAvailable`/`Failed`.
- Tests: `install`, `sources`, `source_install`, `catalog`, `config`, `consent`,
  `disabled_list`, `removed_list`, `seed`, `builtin_root`, `manager`,
  `extension_host`, `extension_scan` (`dlopen` fixtures), `command_codec`, and
  the Qt `extensions_controller` suite. Author guide: `docs/extensions.md`.

## Build / test (exact)

```
# One-time: bootstrap vcpkg and export VCPKG_ROOT (see docs/building.md).
cmake --preset vcpkg
cmake --build build
ctest --test-dir build --output-on-failure                # 125 entries
ctest --test-dir build -R <suite> --output-on-failure     # one suite (e.g. -R document)
```

- Outputs are `build/bin/genesis-cli` and `build/bin/genesis-app`.
- `ctest -N` is authoritative for the count. Extra suites register only with an
  option ON: `whisper_provider`, `tts_provider`, `segmentation_provider`,
  `brush_provider` (AI runtimes), `updater_crypto`/`catalog_crypto`
  (`GENESIS_UPDATER=ON` + OpenSSL), `grpc_serve` (`GENESIS_GRPC=ON`).
- Without Qt6 >=6.5 (GuiPrivate) the build degrades: the Qt controller suites are
  skipped with a warning (`GENESIS_CAN_BUILD_QT_TARGETS`). `genesis_app` is not a
  ctest suite at all (it needs a display).
- `-Wall -Wextra -Wpedantic -Werror` is on every target; keep the build
  warning-free.
- System deps: Boost headers; CLI11, nlohmann-json, pugixml, toml++, libcurl and
  libarchive (vcpkg supplies them through `vcpkg.json`; the `vcpkg` preset is the
  build path); GStreamer 1.x + GES + graphene; EGL/GLES (Mesa llvmpipe for
  headless GL tests). GES ships no `.pc` — it is linked raw as `ges-1.0`.
  GStreamer plugin sets (base/good/bad/libav + GL elements) are needed at test
  time, not just build.
- **Build options that reach into the toolchain**: `GENESIS_CCACHE` (default ON)
  and `GENESIS_LD` (default ON, links `lld` when found). Both fall back quietly,
  but `find_program` caches its result: if `lld` is removed after a configure,
  every relink fails with `collect2: cannot find 'ld'`. Fix with
  `cmake -U GENESIS_LLD_PROGRAM -S . -B build -G Ninja` (or `-DGENESIS_LD=OFF`).
- Format/lint: `.clang-format` (Qt-ecosystem style, 100 columns) and
  `.clang-tidy` are the coding standard; keep them canonical, no hand-tuned
  deviation. **The format checks are ctest suites, not build failures:**
  `cmake --build build --target format` then `ctest -R format_check`;
  `cmake --build build --target qml_format` then `ctest -R qml_format_check`.
  CI enforces both. clang-format is pinned to **22.1.8** (CI installs it; see
  `docs/building.md`). `tidy` and `cppcheck` targets are manual, report-only.
- QML: `.qmlformat.ini`/`.qmllint.ini` use Qt defaults; context properties are
  declared in `.contextProperties.ini`, which must stay in sync with
  `AppShell.cpp` (21 properties). qmlformat output is version-sensitive — it must
  match the Qt the tree was formatted with (6.11). `all_qmllint` is report-only
  with a known set of pre-existing warnings that are NOT a gate; do not whitelist
  new ones.

## Dev/AI workflow commands

`build/bin/genesis-dev` wraps the gate:

```
build/bin/genesis-dev verify               # configure + build + flake-aware ctest + smoke
build/bin/genesis-dev verify --skip-smoke  # build + flake-aware ctest only
build/bin/genesis-dev smoke [--surfaces a,b]   # UI smoke gate
build/bin/genesis-dev suites [<files…>]    # suite -> sources map for targeted ctest
```

- `smoke` opens each surface against a fresh, empty project and asserts **zero QML
  runtime messages** plus a bounded peak RSS. Reports (per-surface PNGs,
  `report.json`) land in `build/ai-smoke/`.
- Surface names come from `tools/dev/classify.cpp` (`known_surfaces`). A surface
  contributed by an extension dialog uses the **extension id**
  (`genesis.interchange`), because the host opens it by id; panel surfaces use a
  short token. Add the token to `known_surfaces` or the tool rejects it.
- The demo fixture is `/tmp/ges-spike/a.webm`; smoke regenerates it with
  `gst-launch-1.0` when absent, so a cleaned temp dir is not a UI regression.
- **Limitation:** smoke uses a fresh empty project, so bugs needing a loaded
  project plus a selection may not reproduce. Those still need full `ctest` or a
  manual load.

## Gated subsystems (all default OFF)

- **AI**: `GENESIS_AI` (master), `GENESIS_AI_CAPTIONS` (whisper.cpp),
  `GENESIS_AI_VISION` (ONNX Runtime), `GENESIS_AI_TTS` (sherpa-onnx),
  `GENESIS_AI_GPU` (CUDA whisper, implies CAPTIONS). Non-default verify configs:
  `build-ai2`, `build-ai3`, `build-aig`.
- Providers run behind the injectable `jobs::Scheduler` seam; app controllers
  inject the engine's decode seams so `genesis_ai` never touches GStreamer or Qt.
- Async AI runs are **project-epoch guarded**: `Editor::epoch()` bumps on every
  `load`; each controller captures it and discards a result computed against a
  swapped-out project (`error() == "project changed"`). The `*_controller` suites
  pin this via `test_project_change_discards` (skipped unless the runtime is on).
- Model installs are SHA-256 verified, streamed to a partial file, atomically
  renamed only on digest match; downloads are consent-gated and land under the
  app's `AppDataLocation/models`.
- **gRPC**: `GENESIS_GRPC=ON` builds `src/cli/GrpcServe`; `genesis-cli serve
  --grpc --grpc-addr <addr> --token <token>` requires `authorization: Bearer`.
  Heavy dependency (find_package, else pinned FetchContent); verify config
  `build-grpc`.
- **Updater**: `GENESIS_UPDATER=ON` builds `src/workspace/update/` (signed
  manifest, Ed25519 via OpenSSL, stage + atomic swap). `kReleasePublicKey` is all
  zeros, so even an enabled build verifies nothing. App surface is notify +
  explicit-confirm apply on Linux; verify config `build-upd`.
- `src/app/Version.h` holds `kAppVersion` from the `GENESIS_APP_VERSION` define;
  its fallback must stay in sync with `PROJECT_VERSION` on a bump.

## Adding code / tests

- Sources are EXPLICIT lists — no globs. A new `.cpp` MUST be added to
  `GENESIS_HOST_SOURCES` (or the target's source list) in `CMakeLists.txt`.
  Header-only additions need no CMake change.
- Register tests with `genesis_add_test(NAME <n> SOURCES ... LIBRARIES ...
  [INCLUDE_DIRECTORIES ...] [COMPILE_OPTIONS ...] [WORKING_DIRECTORY <dir>])`;
  add warnings with `genesis_warnings(target)`.
- Suites that read `assets/`, the builtin packs, or fixture files set
  `WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}` and locate their files by
  walking up from the current directory. Follow that pattern; a new
  asset-reading suite that omits it fails only when run from `build/`.

## Fragile contracts (change with care)

- On-disk project JSON is a contract: never rename keys or enum spellings; the
  `document` suite pins round-trip and the current shape (`DOCUMENT_VERSION` in
  `src/project/Document.h`).
- Adding/removing a Command touches four places together:
  `src/project/command/Command.h` (variant), `src/project/commands/Apply.cpp`
  (dispatcher), `src/project/command/Validate.h`, and the group `*Commands.h` +
  verb `.cpp`.
- QML context property names are stringly coupled (`controller`, `timelineModel`,
  `edit`, `exporter`, `mediaBin`, `keyframes`, `scopes`, `settings`, `project`,
  `i18n`, `effectParams`, `gpu`, `updates`, `extensions`, `dialogBridge`, …);
  renaming one breaks QML at load. It is `exporter`, not `export` (reserved).
- Test pins on shipped assets: 20 title SVGs / 47 slots, 8 `.cube` LUTs, and the
  effect count (`kExpectedPacks = 123` in `named_intermediate`). `lut`-named enum
  params resolve by index into the sorted `assets/luts/*.cube` list (see
  `src/effects/Resolve.h`). Update the pinning tests when assets change.
- Reverse/proxy cache filenames are an on-disk contract (FNV-1a stem of the source
  path, plus the span for reverse); changing the scheme invalidates existing
  caches (see `src/render/{Reverse,Proxy}Cache.h`).
- QML `Theme.qml` is a singleton (`QT_QML_SINGLETON_TYPE`); `src/app/model` must
  stay on the app target's include path.
- In app code, include GStreamer/GES headers BEFORE Qt headers — glib's `signals`
  field collides with the Qt `signals` macro.
- `genesis_ai` stays engine-free and Qt-free: no GStreamer/GES or Qt type may
  cross into it. The decode seam lives in the engine adapter (`ges/media/Probe.h`,
  `ges::extract_video_frames`/`extract_audio`) and is injected through
  `jobs::Scheduler`.
- GL test suites create their own EGL context via GStreamer GL elements. CI runs
  `xvfb-run` + `LIBGL_ALWAYS_SOFTWARE=1` + llvmpipe and deliberately does NOT set
  `EGL_PLATFORM`. `x264enc` is absent on the dev host — use VP8/VP9/OpenH264/
  Theora for media fixtures.
- Document/JSON loads are bounded via `src/core/JsonGuard.h` (64 MiB, depth 512);
  raise the bound consciously — it guards project, template, manifest and RPC
  paths.

## Accessibility (QML)

Interactive QML carries Qt Quick `Accessible` metadata: design-system widgets
(`Button`, `ToggleButton`, `InputField`, `LabelledSlider`) set `Accessible.role`
(plus `name`/`value`/`checked`), and custom controls (timeline, transport, stroke
overlay) set a role and description. Preserve these when reworking a control —
a control with no role is invisible to assistive technology, and qmllint does not
flag its absence.

## Repo state notes

- `docs/` is git-tracked: `README.md`, `architecture.md`, `building.md`,
  `dependencies.md`, `extensions.md`, `decisions/` (ADRs), `history/` (executed
  plans), `proposals/` (open designs).
- License is **GPL-3.0-or-later** (SPDX headers + `LICENSE`). Shipped assets are
  generated in-house, no third-party rights. Do not mass-edit license text or
  headers without a deliberate decision.
- `extensions/builtin/` holds the builtins; `genesis.packs` is data (123
  `[[effect]]` entries under `packs/genesis.*`). `spikes/` are standalone
  experiments with their own builds; `examples/` holds standalone Extension SDK
  examples. Neither is part of the main CMake build.
- The CLI vets a candidate pack without executing it: `genesis-cli packs trial
  <dir>` (add `--validate-only`) parses, validates and compiles the fragment, then
  reports the shader outcome. It is a trial, not a trust decision.
- `src/project/` layout: `Project.h` (edit model) + `model/` (data types),
  `command/` (Command variant + group structs + Validate), `document/`
  (`Document.cpp` public API; Json/Read/Settle/Write internals), `Naming.h`,
  `commands/` (verb handlers + Apply dispatcher).
- API surface: `src/api/{Api,Codec,Requests,LocalPath,TextFile}` — request
  dispatch, wire codec, request types, path normalisation and file I/O are
  separate files; put new helpers in the matching one rather than growing
  `Api.cpp`.
