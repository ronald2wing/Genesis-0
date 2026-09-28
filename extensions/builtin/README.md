# Builtin extensions

Each subdirectory here is one **builtin extension**, named by its id
(`<id>/`). A builtin is first-party code that ships with the host: a native
extension (`extension.toml`) or a Pack (`effect.toml`). It is trusted (origin
`builtin`) and loads without consent.

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
manifest-validated builtin whose id is not already installed and not on the
store's `removed.json` tombstone list is copied in and recorded as origin
`builtin`. The seed is idempotent, and a tombstoned id is skipped forever until
the user restores or reinstalls it (`src/extensions/Seed.cpp`).

The directory may hold a README and no builtins yet; builtins are added as
subdirectories as they ship.

The one shipped builtin is `genesis.packs`, a data-only native extension: it
has no `entry` (no library), and its `extension.toml` lists the 83 official
pack ids it bundles under `packs/`. Seeding copies the extension — including
that pack payload — into `<store>/genesis.packs/<version>/`, and its `packs/`
directory is the catalogue's official pack root.
