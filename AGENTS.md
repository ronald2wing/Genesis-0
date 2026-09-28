# AGENTS.md

Repo-specific instructions for coding agents. Universal rules (commit
discipline, style) live in the global AGENTS.md and are not repeated here.

## Stack

- C++23 cross-platform video editor. CMake >=3.28 + Ninja.
- Include root is `src/`, so includes look like `#include "project/Project.h"`.
- Targets:
  - `genesis_host` — engine-free, Qt-free core: `src/core`, `project`,
    `render`, `effects`, `extensions`, `jobs`, `api`, `workspace`, plus the
    engine-free half of `src/ai` (`ModelManifest`, `ModelStore`, `Sha256`,
    `vision/MaskStore`).
  - `genesis_ai` — AI provider library; engine-free and Qt-free, links
    `genesis_host`, always built. `src/ai/{captions,vision,tts}` providers
    report `disabled` when their runtime is not compiled in.
  - `genesis_engine_ges` — the GES adapter library; the only *library* that
    links GStreamer/GES. Subdirs `ges/{session,effects,media,scopes}` plus the
    top-level `Export.cpp`, `Encoder.{h,cpp}`, `Hardware.{h,cpp}`;
    `ges/media/AudioFrames.{h,cpp}` and the media seam (`Probe`, `VideoFrames`)
    live under `ges/media/`. `ges/Hardware.{h,cpp}` is the bounded hardware
    probe (GL renderer, software GL, hardware decode/encode reachability); the
    app projects it through the `HardwareStatus` controller exposed to QML as
    the `gpu` context property.
  - `genesis_app` — the only Qt + GES target; needs Qt6 >=6.5.
  - `genesis_cli` — host CLI; Qt-free, links the engine adapter for `render`.
    `src/cli/` holds `SocketServe.{h,cpp}` (Unix-socket transport),
    `GrpcServe.{h,cpp}` (gRPC transport, gated), `JsonRpc.{h,cpp}` (shared
    JSON-RPC codec) and `Commands.{h,cpp}` (the verbs).
  - tests, `perf_tests`.
- Seam headers app/host talk to: `src/adapters/engine/{EngineSession,Export,Scopes}.h`.
- `src/app/AppShell` is the composition root (`src/app/GenesisApp.cpp` is the
  thin entry); controllers are one class per file under `src/app/controllers/`,
  the QML read-only projections live in `src/app/model/`, QML is a flat
  `src/app/qml/` dir, and the net/support/render helpers live under
  `src/app/{net,support,render}/` (the root holds only `GenesisApp.cpp`,
  `AppShell.{h,cpp}`, and `Version.h`); `src/workspace/`
  is the app-service layer
  (Projects/Templates/Logs), NOT the engine host core.
- `genesis::project::Editor` owns the project and swaps it in place on load —
  app controllers must hold the `Editor*`, not pointers into the project.

## Extension naming

- The umbrella term is **extension**. The two kinds are **packs** (declarative
  data/GLSL effect bundles — the official set ships as the builtin
  `genesis.packs` extension under `extensions/builtin/genesis.packs/packs/`)
  and **native extensions** (shared
  libraries contributing native code/UI). Use "extension" for this product
  surface; reserve "plugin" for technical framework objects (Qt/GStreamer/GES/
  MLT elements, a manifest `kind` field). Do not call this system a "module" or
  a "mini-app".

## Extension system (native SDK, host-side)

- Layout under `src/extensions/` (all in `genesis_host`, Qt-free/engine-free):
  - `ExtensionApi.h` — the public C99 ABI a native extension is written against
    (`genesis_extension_entry`, append-only `size`-negotiated structs;
    `GENESIS_EXTENSION_API_VERSION`).
  - `NativeManifest.{h,cpp}` — parses `extension.toml` (identity, library entry,
    menus/panels/packs) without loading code; `entry`/panel `qml` must be bare
    file names.
  - `NativeExtension.{h,cpp}` — POSIX `dlopen`/`dlsym` loader; a library is never
    unloaded in v1. Injected `Host` seams route `call_json` (to
    `api::dispatch`) and `log`.
  - `ExtensionHost.{h,cpp}` — scans the install store, picks the highest version
    per id, applies the origin/consent/disable gates, and collects
    menus/panels/pack roots.
  - `Install.{h,cpp}` — validates the manifest, applies the trust policy, and
    installs into `store_root/<id>/<version>`; refuses without writing on
    failure.
  - `Trust.h` — `Origin{Builtin,Curated,Developer,User}` and `is_trusted`/
    `may_load_native`: Builtin/Curated load freely; Developer is visible but
    consent-gated; User is refused.
  - `Consent.{h,cpp}` — the `<store>/developer-consent.json` record that turns a
    Developer extension loadable; absent/unreadable is empty, writes atomic.
  - `DisabledList.{h,cpp}` — `<store>/disabled.json`, the enable/disable switch
    for Builtin/Curated ids (Developer uses consent instead).
  - `Origins.{h,cpp}` — `<store>/origins.json`, the id -> origin record read at
    scan and written at install (absent id defaults to Developer).
- The host API bridge is `src/api/` (`Api.{h,cpp}`, `Codec`, `Requests`); the
  `edit.apply` JSON-RPC verb carries a project command (pinned by the
  `command_codec` suite). The management methods are `extensions.list`,
  `extensions.remove`, `extensions.enable`, `extensions.disable` and
  `extensions.restore`; the CLI verbs are `genesis-cli extensions
  list/install/catalog/trust/remove/restore/enable/disable`, over the same store.
- Install **sources** (`Sources.{h,cpp}`, `SourceInstall.{h,cpp}`): a local
  **directory** (the app's picker, or `extensions install <dir>`), a **git** repo
  (`extensions install --git <url> [--ref <ref>] [--subdir <subdir>]`, shelling
  out to `git clone --depth 1`), and a **catalog** (`extensions catalog <url>`
  lists it, `extensions install --catalog <url> <id>` installs one entry) via
  `Catalog.{h,cpp}`. Every id's source is recorded in `<store>/sources.json` for
  config export. A source never elevates trust: the default origin is `user`
  (refused), so pass `--origin developer` (consent-gated) or `curated`. The CLI's
  catalog fetcher shells out to `curl`; `--store <dir>` overrides the default
  store.
- **Config profiles** (`Config.{h,cpp}`): a portable versioned JSON snapshot of
  the portable settings subset plus the installed extensions with their origin
  and install source. `config.export` returns the document; `config.import`
  takes `params.profile` and reports per-setting and per-extension outcomes.
  CLI: `genesis-cli config export <file>` / `config import <file>
  [--no-install]`. Import never elevates trust: already-installed, sourceless,
  and `builtin` entries are skipped; a `dir`/`git` source from a `developer`
  origin installs consent-gated; a `curated`/`builtin` origin with such a source
  is skipped; `user` fails. Settings travel through an injected `ConfigSeam` —
  the app wires it over `AppSettings`/`UpdateController`, the headless CLI wires
  none (every setting omitted/skipped).
- **Served-method split.** The app's `Services` (`AppShell.cpp`) wires extension
  management (`extensions.*`), `config.export`/`config.import`,
  `extensions.catalog`, plus `export.run`/`export.cancel`/`export.status`,
  `scopes.snapshot`, `edit.selection`, `preview.time`, `media.list` and
  `gpu.status`. The CLI (`cli_services`) wires only management + config +
  catalog; probes/catalogue and the export/scope/preview services stay unwired
  there and `dispatch` refuses them as `NotAvailable`/`Failed`.
- **Project bridge.** Panels and the Extensions dialog reach the host through the
  `extensions` context property's `call(method, paramsJson)` invokable (QML
  `extensions.call`), which routes to the same `api::dispatch` JSON-RPC surface
  as `call_json` and returns the reply envelope. `ExtensionsDialog.qml` uses it
  for the install picker, the in-app **marketplace** (`extensions.catalog` +
  `extensions.install`) and the **config profile** export/import sections
  (`config.export`/`config.import`).
- Manifest is `extension.toml`, **format 2** (format 1 still reads; >2 is
  refused). Format 2 adds `[extension].requires = ["author.dep", ...]`
  (namespaced ids this extension depends on). The scan Kahn-sorts the graph and
  loads in topological order: a cycle fails its members with `dependency cycle:
  …`, a missing dependency fails the dependent with `requires <id>`, and a
  disabled/failed dependency fails the dependent the same way. Disabling or
  removing an extension cascades to its transitive dependents, recording
  `requires <parent>` as the reason (`Manager`).
- Builtins live under `extensions/builtin/` (copied beside the CLI binary at
  build time; `Seed.{h,cpp}` seeds them into the store on each start). A builtin
  is user-removable: removing it writes its id to the store's `removed.json`
  tombstone and deletes the installed copy, and the seed skips a tombstoned id
  forever (hidden, never silently re-added). `extensions restore <id>` clears
  the tombstone and re-seeds, and reinstalling the same id also clears it
  (`Install.cpp`). The `extensions.remove` API method refuses a Builtin (the
  app's `ExtensionsController` has a dedicated tombstone path); the CLI and app
  dialog remove them directly.
- Two official builtins ship: `genesis.packs`, the data-only extension whose
  `packs/` holds the 83 `packs/genesis.*` effect bundles and is the catalogue's
  official pack root (`Catalogue::source_root()` is the dev fallback); and
  `genesis.titles`, the first tool converted to an extension — a data-only
  panel (`TitlesPanel.qml`) that replaces the old `TitleGalleryDialog` and
  reaches the host through the `extensions.call` bridge (`template.list` /
  `template.fill` / `edit.apply`).
- Extension panels import the host module with `import GenesisApp 1.0` (the app
  QML module registered on the standard `/qt/qml` resource prefix), because a
  panel sits in the store outside the app's QML directory; host context
  properties (`extensions`, `controller`, `timelineModel`, …) arrive on the
  panel's context.
- Tests: `install`, `sources`, `source_install`, `catalog`, `config`,
  `consent`, `disabled_list`, `removed_list`, `seed`, `manager`,
  `extension_host`, `extension_scan` (host-side, `dlopen` fixtures),
  `command_codec` (the `edit.apply` codec pin), and the Qt
  `extensions_controller` suite. Author guide: `docs/extensions.md`.

## Build / test (exact)

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure                # all 99 suites
ctest --test-dir build -R <suite> --output-on-failure     # single suite (e.g. -R document)
```

- Build outputs are `build/bin/genesis-cli` and `build/bin/genesis-app`
  (runtime output dir is `build/bin` for both).
- 99 suites = 83 non-Qt + 16 Qt (default build); all display-free. `genesis_app`
  is NOT a ctest suite (it needs a display). Four more suites are registered
  only with an AI runtime ON (`whisper_provider`, `tts_provider`,
  `segmentation_provider`, `brush_provider`), `updater_crypto` only with
  `GENESIS_UPDATER=ON` and OpenSSL found, and `grpc_serve` only with
  `GENESIS_GRPC=ON`.
- Without Qt6 >=6.5 (GuiPrivate) the build degrades: the 16 Qt controller suites
  are skipped with a warning (`GENESIS_CAN_BUILD_QT_TARGETS`).
- `-Wall -Wextra -Wpedantic -Werror` is on every target via helper; keep the
  build warning-free.
- System deps: Boost headers; GStreamer 1.x + GES + graphene; EGL/GLES (Mesa
  llvmpipe for headless GL tests). GES ships no `.pc` — it is linked raw as
  `ges-1.0`. GStreamer plugin sets (base/good/bad/libav + GL elements) are
  needed at test time, not just build.
- Formatting/linting: `.clang-format` (style) and `.clang-tidy` (checks) are the
  official coding standard, and the durable rule is that **lint/format settings
  stay the canonical, popular ones** — no hand-tuned deviation. clang-format is
  the **Qt-ecosystem style** — a port of Qt's canonical `_clang-format` (100
  columns, pointer/reference on the right, Qt brace placement), the most common
  style for Qt6 projects (Qt/Qt Creator/KDE/MLT). clang-tidy runs the canonical
  set from the most-used clang-tidy CI action, `ZedThree/clang-tidy-review`
  (`performance-*`, `readability-*`, `bugprone-*`, `clang-analyzer-*`,
  `cppcoreguidelines-*`, `mpi-*`, `misc-*`). Run the formatter after every
  change (`cmake --build build --target format`); the `format_check` suite must
  pass — CI enforces it. `cmake --build build --target tidy` is a manual,
  report-only clang-tidy pass. `cmake --build build --target cppcheck` is the
  optional, report-only third tool (clang-format + clang-tidy + cppcheck) that
  runs cppcheck's standard `--enable=warning,performance,portability`
  categories; run it when touching risky code. All are gated (absent tool →
  configure warning → skip), and clang-format is pinned to 22.1.8 (see
  `docs/building.md`).
- QML formatting/linting: `.qmlformat.ini` (style) and `.qmllint.ini` both use
  their Qt defaults (qmllint's default severities; context properties in
  `.contextProperties.ini`). Run
  `cmake --build build --target qml_format` after QML changes; the
  `qml_format_check` suite must pass — CI enforces it. `all_qmllint` is the
  report-only lint target (auto-generated by `qt_add_qml_module`); it emits a
  known set of pre-existing `unqualified-access`/`missing-property` warnings
  (~369/~224) that are NOT a gate. Do not whitelist new ones, and keep the
  `.contextProperties.ini` list in sync with `AppShell.cpp` (18 properties).
  qmlformat output is version-sensitive — it must match the Qt the tree was
  formatted with (6.11). Both gated (absent tool → configure warning → skip);
  see `docs/building.md`.

## AI subsystem (gated, default OFF)

- `CMakeLists.txt` options, all default OFF so a default checkout is
  dependency-free: `GENESIS_AI` (master), `GENESIS_AI_CAPTIONS` (whisper.cpp),
  `GENESIS_AI_VISION` (ONNX Runtime), `GENESIS_AI_TTS` (sherpa-onnx); each
  runtime option implies `GENESIS_AI`.
- `genesis_ai` is engine-free and Qt-free and always built; its providers
  report `disabled` when their runtime is not compiled in, so the app links one
  target and asks it what it can do at runtime. Runtime code is guarded by
  `GENESIS_AI_*_ENABLED`.
- `src/ai/` layout: `ModelManifest`/`ModelStore`/`Sha256` (host-side model
  store), `captions/` (WhisperProvider), `vision/` (SegmentationProvider for
  RVM + IS-Net, EnhanceProvider for Real-ESRGAN, BrushProvider for the SlimSAM
  encoder + prompt/mask decoder, MaskStore, MaskDriver), `tts/`
  (SherpaTtsProvider).
- Providers run behind the injectable `jobs::Scheduler` worker seam; the app
  controllers `AiController`/`CutoutController`/`EnhanceController`/
  `TtsController` drive them. Injection keeps the providers testable with
  fakes and the app's frame/audio source reaches the engine adapter only via
  injected seams.
- Model installs: SHA-256 verified, streamed to a partial file and atomically
  renamed only on digest match, with a licence/digest sidecar; a download with
  no digest is refused. Downloads are consent-gated and land under the app's
  `AppDataLocation/models`, never the cache root.
- `GENESIS_AI_GPU=ON` builds whisper.cpp's CUDA kernels when
  `find_package(CUDAToolkit)` finds the toolkit; without it the option warns and
  whisper stays on CPU. It implies `GENESIS_AI_CAPTIONS`. ONNX Runtime
  execution providers are detected at runtime: CUDA is used when ORT reports it,
  otherwise the session falls back to CPU (`src/ai/vision/ExecutionProvider.h`, suite
  `execution_provider`).
- Non-default verify configs: `build-ai2` (`GENESIS_AI_VISION=ON`),
  `build-ai3` (`GENESIS_AI_TTS=ON`) and `build-aig` (`GENESIS_AI_GPU=ON`,
  configure-only when no CUDA toolkit is present); the default `build` has every
  AI runtime OFF.

## gRPC transport (gated, default OFF)

- `GENESIS_GRPC=ON` (default OFF) builds the gRPC server transport in
  `src/cli/GrpcServe.{h,cpp}`. It reuses the JSON-RPC codec of the stdio `api`
  verb and the Unix-socket `serve` verb; every Call carries one JSON-RPC 2.0
  request and returns one reply. `GENESIS_GRPC_ENABLED` is set only when the
  option is ON and gRPC actually resolves.
- gRPC is a heavy dependency (tens of minutes on a cold build). Configure first
  tries `find_package(gRPC CONFIG)`, then falls back to a pinned `FetchContent`
  (`v1.82.0`). The default checkout neither fetches nor links it.
- `genesis-cli serve --grpc --grpc-addr <addr> --token <token>` serves over gRPC
  instead of the Unix socket; `--grpc` requires a non-empty `--token`, and every
  call must present `authorization: Bearer <token>`. Without the option the
  `--grpc` flags are not registered and the verb stays host-only.
- The `grpc_serve` suite is registered only with `GENESIS_GRPC_ENABLED`; the
  non-default verify config is `build-grpc`.

## Self-update core (gated, default OFF)

- `GENESIS_UPDATER=ON` (default OFF) builds the self-update core in
  `src/workspace/update/`: `Manifest.{h,cpp}` (signed release manifest),
  `Signature.{h,cpp}` (Ed25519 verify via OpenSSL EVP), `Updater.{h,cpp}`
  (verify -> download -> stage -> atomic swap, with `confirm`/`rollback`).
  The default checkout still compiles the sources but reports `disabled`:
  the verifier resolves to `Unavailable` and every install is refused before
  a byte is fetched.
- `GENESIS_UPDATER_ENABLED` is set only when the option is ON *and* OpenSSL
  is found; without OpenSSL the option warns and the updater stays disabled.
  The embedded `kReleasePublicKey` is all zeros until a release key is minted,
  so even an enabled build verifies nothing.
- **The app surface is notify-only** — `Settings` -> `Updates`
  (`UpdateController`, context property `updates`) checks the feed and tells the
  user when a newer version exists; it never applies. The apply path is still
  deferred (`docs/decisions/auto-update.md`).
- The running app version lives in `src/app/Version.h` as `kAppVersion`,
  compiled from the `GENESIS_APP_VERSION` define CMake passes the app target;
  the header's fallback must stay in sync with `PROJECT_VERSION` on a bump.
- `update_controller` is the notify-only surface's hermetic suite (stub feed
  fetcher + signature verifier + gate); it is a Qt-controller suite and runs in
  the default build.
- `build-upd` (`GENESIS_UPDATER=ON`) is the verify config; its extra suite is
  `updater_crypto` (Ed25519 sign/verify, seeded in-process; no key on disk).

## Adding code / tests

- Sources are EXPLICIT lists — no globs. A new `.cpp` MUST be added manually to
  `GENESIS_HOST_SOURCES` (or the target's source list) in `CMakeLists.txt`.
- Register tests with `genesis_add_test(NAME <n> SOURCES ... LIBRARIES ...
  [INCLUDE_DIRECTORIES ...] [COMPILE_OPTIONS ...] [WORKING_DIRECTORY <dir>])`;
  add warnings with `genesis_warnings(target)`.
- Tests reading the builtin packs or `assets/` use a walk-up-from-cwd locator.
  11 suites
  set `WORKING_DIRECTORY` to the source dir (pack_source, catalogue, reveal_pack,
  pack_batch, effect_application, clip_effect, chain_consumption,
  named_intermediate, title_templates, presets, effect_params_controller).
  Follow that pattern for new fixture-reading tests.

## Fragile contracts (change with care)

- On-disk project JSON is a contract: never rename JSON keys or enum spellings;
  the `document` suite pins round-trip and the v1 shape. Migrations live in
  `src/project/document/Migrate.cpp` (`DOCUMENT_VERSION`).
- Adding/removing a Command touches 4 places together:
  `src/project/command/Command.h` (variant list),
  `src/project/commands/Apply.cpp` (dispatcher),
  `src/project/command/Validate.h` (`has_non_finite`/`is_view_state`),
  and the group `*Commands.h` + verb `.cpp`.
- QML context property names are stringly coupled (`controller`,
  `timelineModel`, `edit`, `exporter`, `mediaBin`, `keyframes`, `scopes`,
  `settings`, `project`, `i18n`, `effectParams`, `gpu`,
  `updates`, `extensions`); renaming
  one breaks QML at load. It is `exporter`, not `export` (reserved word). `gpu`
  is the `HardwareStatus` projection (`src/app/qml/GpuIndicator.qml`); `updates`
  is the `UpdateController` projection (`src/app/qml/SettingsDialog.qml`);
  `extensions` is the `ExtensionsController` native-extension/panel projection
  (`src/app/qml/ExtensionsDialog.qml`).
- Test pins on shipped assets: 20 title SVGs / 47 slots, 8 `.cube` LUTs (plus a
  hardcoded `teal_orange.cube`), and pack counts asserted per-suite
  (`pack_source`, `reveal_pack`, `pack_batch`, `named_intermediate` each pin the
  full 83 builtin pack dirs under `extensions/builtin/genesis.packs/packs/`);
  `lut`-named enum params resolve by index into the
  sorted `assets/luts/*.cube` list (see `src/effects/Resolve.h`).
  Update the pinning tests when assets change.
- Reverse/proxy cache filenames are an on-disk contract: the FNV-1a stem of the
  source path (proxy) or source path + span (reverse); changing the key scheme
  invalidates existing caches (see `src/render/{Reverse,Proxy}Cache.h`).
- QML `Theme.qml` is a singleton (needs `QT_QML_SINGLETON_TYPE`); `src/app/model`
  (where the QML_ELEMENT `MonitorItem.h` lives) must stay on the app target's
  include path.
- In app code, include GStreamer/GES headers BEFORE Qt headers — glib's
  `signals` field collides with the Qt `signals` macro.
- `genesis_ai` stays engine-free and Qt-free: no GStreamer/GES or Qt type may
  cross into it. The media decode seam lives in the engine adapter (`probe` at
  `src/adapters/engine/ges/media/Probe.h`, frame/audio extraction at
  `ges::extract_video_frames`/`extract_audio`); app controllers inject those
  into `src/ai` providers through the `jobs::Scheduler` worker seam. A provider
  that reaches the engine directly would break the host/engine split.
- GL test suites create their own EGL context via GStreamer GL elements. CI runs
  `xvfb-run` + `LIBGL_ALWAYS_SOFTWARE=1` + llvmpipe and deliberately does NOT
  set   `EGL_PLATFORM`. CI covers 83/99 (Qt skipped: Ubuntu 24.04 ships Qt 6.4).
  `x264enc` is absent on the dev host — use VP8/VP9/OpenH264/Theora for media
  fixtures.
- Document JSON loads are bounded via `src/core/JsonGuard.h` (64 MiB, depth 512);
  raise the bound consciously — it guards project/template/manifest/RPC paths.

## Repo state notes

- `docs/` (19 files: architecture.md, building.md, dependencies.md,
  extensions.md, implementation-plan.md, rewrite-plan.md, decisions/*,
  plans/*) is git-tracked.
- License is **GPL-3.0-or-later** (SPDX headers + `LICENSE` full text). The
  shipped assets are generated in-house, no third-party rights. Do not mass-edit
  license text or headers without a deliberate user decision.
- `extensions/builtin/` holds the builtin extensions; `genesis.packs` is data
  (83 effect-pack dirs, all `packs/genesis.*`: TOML manifest +
  GLSL); `spikes/` are standalone experiments with their own builds; `examples/`
  holds standalone Extension SDK examples (e.g. `examples/hello-extension/`,
  with its own CMakeLists). None is part of the main CMake build. Author guide:
  `docs/extensions.md`.
- The CLI vets a candidate Pack without executing it: `genesis-cli packs trial
  <dir>` (add `--validate-only` to stop after manifest validation) parses,
  validates and compiles the fragment via `src/cli/Commands.cpp`, then reports
  the shader outcome. It is a trial, not a trust decision.
- `src/project/` layout: `Project.h` (edit model) + `model/` (data types),
  `command/` (Command variant + group structs + Validate), `document/`
  (Document.cpp public API; Json/Read/Write/Migrate/Settle internals),
  `Naming.h` (name/series helpers), `commands/` (verb handlers + Apply
  dispatcher).
