# Builtin extensions

Each subdirectory here is one **builtin extension**, named by its id
(`<id>/`). A builtin is first-party code that ships with the host, described
by its `extension.toml`. It is trusted (origin `builtin`) and loads without
consent.

## Packaged layout

At build time CMake copies this directory beside the binaries
(`build/bin/extensions/builtin/`). At runtime the app and CLI resolve the
builtin root in this order (`src/extensions/Seed.cpp` `builtin_root()`):

1. the `GENESIS_BUILTIN_EXTENSIONS_DIR` compile-time define (this source
   directory, so a developer build needs no install step);
2. a search beside the running executable (`<exe>/extensions/builtin/`), the
   packaged layout.

## Seeding

On startup (and via `genesis-cli extensions restore <id>`), the host seeds the
installed-extension store (`<store>/<id>/<version>`) from this directory: a
manifest-validated builtin is copied in and recorded as origin `builtin` when
its `version` is newer than anything installed under that id, so shipping a
changed builtin needs a `version` bump in its `extension.toml`. An id on the
store's `removed.json` tombstone list is skipped forever until the user
restores or reinstalls it (`src/extensions/Seed.cpp`).

Nine builtins ship today, all data-only (no `entry`): one pure-data pack
bundle, five panels and three dialogs:

- `genesis.packs` — its `extension.toml` inlines the 123 official effects as
  `[[effect]]` blocks whose `body` fields name GLSL sources under `packs/`.
  Seeding copies the extension — including that pack payload — into
  `<store>/genesis.packs/<version>/`, and its `packs/` directory is the
  catalogue's official pack root.
- `genesis.titles` — a `TitlesPanel.qml` panel for the title-template library
  (`template.list` / `template.fill` / `edit.apply` over `extensions.call`).
- `genesis.scopes` — a `ScopesPanel.qml` panel fed by the `scopes.snapshot`
  host method, which it polls through `extensions.call`.
- `genesis.export` — an `ExportPanel.qml` panel, a port of the old export
  dialog, using the `export.presets` / `export.run` / `export.status` /
  `export.cancel` host methods through `extensions.call`.
- `genesis.captions` — a `CaptionsDialog.qml` dialog, a port of the old caption
  overlay, using the `ai.models` / `ai.download` / `ai.status` / `ai.run` /
  `ai.cancel` and `edit.selection` host methods through `extensions.call`.
- `genesis.speech` — a `SpeechDialog.qml` dialog, a port of the old
  text-to-speech overlay, using the same AI methods through `extensions.call`,
  plus the `ai.models` voice list to feed its voice picker.
- `genesis.cutout` — a `CutoutPanel.qml` panel for the AI cutout tool, driving
  the `cutout` controller and the AI host methods through `extensions.call`.
- `genesis.enhance` — an `EnhancePanel.qml` panel for the AI enhance tool,
  using the same AI host methods through `extensions.call`.
- `genesis.interchange` — an `InterchangeDialog.qml` dialog that picks a path
  and calls the `interchange.export` / `interchange.import` host verbs; the
  format logic stays in C++ so the same verbs work headlessly.
