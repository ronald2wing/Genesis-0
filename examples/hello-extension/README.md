# Hello Extension

The smallest useful native Genesis-0 Extension: a shared library that logs
through the host, answers one `invoke` action, contributes one declarative menu
entry, and contributes one QML panel. It is a standalone example — it is **not**
part of the main CMake build.

This example is referenced by [`docs/extensions.md`](../../docs/extensions.md),
which is the full author guide (C ABI, manifest reference, trust model).

## Layout

```
hello-extension/
  extension.toml   the manifest (identity + capabilities)
  hello.c          the library, implementing genesis_extension_entry
  HelloPanel.qml   the panel the extension contributes
  CMakeLists.txt   a standalone build for the library
```

## Build the shared library

Requires CMake >= 3.28, Ninja, and a C compiler. The build reads the SDK header
from the checkout (`../../src`); override with `-DGENESIS_SRC=...` if building
out of tree.

```sh
# Linux / macOS
cmake -S . -B build -G Ninja
cmake --build build
```

On Windows (from a Developer Command Prompt):

```bat
cmake -S . -B build -G Ninja
cmake --build build
```

The library is written **beside** `extension.toml` (the manifest parser requires
`entry` to be a bare file name next to the manifest, not a path):

| Platform | Output | `entry` in `extension.toml` |
|---|---|---|
| Linux | `libhello.so` | `entry = "libhello.so"` |
| macOS | `libhello.dylib` | `entry = "libhello.dylib"` |
| Windows | `libhello.dll` | `entry = "libhello.dll"` |

Edit `extension.toml`'s `entry` to match your platform before installing.

## Install

The default origin is `user`, which policy refuses. Install as a **developer**
extension (unsigned, local-only, load-gated by consent):

```sh
genesis-cli extensions install --origin developer examples/hello-extension
```

The store defaults to `$GENESIS_EXTENSIONS_DIR`, else
`$XDG_DATA_HOME/genesis/extensions`, else `~/.local/share/genesis/extensions`.
Pass `--store <dir>` to target another store. Verify the install:

```sh
genesis-cli extensions list --store <dir>
```

## Grant consent

Installing a developer extension makes it *visible* but not yet *loadable*. The
host refuses to load an unsigned developer extension until the user records
consent for that id (`example.hello`). In the app this is the one-time prompt in
the Extensions panel; consent is persisted in the store as:

```json
{ "ids": ["example.hello"] }
```

at `<store>/developer-consent.json`. (The CLI's `extensions` command currently
exposes `list`, `install`, and `trust` only — there is no `consent` verb yet, so
in a CLI-only workflow you grant consent through the app UI, or write the id
into that file yourself.) `genesis-cli extensions trust developer` shows the
policy line for the origin.

## Where it appears

After consent, the host loads `libhello` and surfaces its capabilities:

- **Tools → Hello** — calls the library's `invoke("hello")`, which logs through
  the host and returns JSON.
- **Tools → Hello Host Version** — a declarative `call`; the host routes
  `version` to its own API bridge and never calls the library.
- **Hello panel** — `HelloPanel.qml`, loaded into the extension panel area.

## Debugging

Diagnose problems from the **host log**:

- The host logs every refusal (bad ABI, id mismatch, missing consent, an
  `invoke` that returned an error) with the reason.
- The extension's own `log` messages (via the `log` callback in
  `GenesisHostV1`) appear in the same log. `hello.c` logs on
  `invoke("hello")`, on project open, and on shutdown.
- A manifest that fails to parse is reported with the offending key and line;
  run the install command and read the printed problems.

There is no debugger attach story beyond ordinary native debugging of the host
process: the library runs in-process, fully trusted. See the security model in
`docs/extensions.md`.

## Notes and known differences from the SDK sketch

This example follows the **parser as it exists today**
(`src/extensions/NativeManifest.cpp`), which is stricter than an early SDK
sketch in two ways:

1. `entry` and a panel's `qml` must each be a **bare file name beside the
   manifest**. A path with a separator (`lib/libhello`, `qml/HelloPanel.qml`) is
   rejected, so the library and `HelloPanel.qml` live next to `extension.toml`.
2. A menu's `call` is parsed as a **string** (the host API method name), not an
   inline table. `call = "version"` parses; `call = { method = "version", ... }`
   does not. See `docs/extensions.md` for the capability reference.
