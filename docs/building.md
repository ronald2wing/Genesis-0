# Genesis-0 — Building

How to build and test Genesis-0 from a fresh checkout. Developers build; end
users do not (see "For users" at the bottom).

Status: the commands below are **verified on the dev host** (Arch, Qt 6.11.2,
GStreamer/GES 1.28.6, GCC 16, CMake 4.4, Ninja 1.13) and **expected** on
Debian/Ubuntu (package names inferred from the Debian mapping, not run). Checked:
2026-09-29. Companion evidence: `docs/dependencies.md`, `CMakeLists.txt`.

## Prerequisites

The build needs exactly these system packages; everything else is fetched and
version-pinned by CMake (see "The FetchContent policy" in `docs/dependencies.md`).

| Requirement | Floor | Notes |
|---|---|---|
| C++23 compiler | GCC 12+ / Clang 16+ | `CMAKE_CXX_EXTENSIONS OFF`; sources use `std::expected`/`std::unexpected` (GCC 12+). Dev host: GCC 16. |
| CMake | ≥ 3.28 | `cmake_minimum_required(VERSION 3.28)`. |
| Ninja | any recent | The documented generator; Make also works. |
| pkg-config | — | Discovered via `find_package(PkgConfig)`. |
| Boost | any recent | Header-only (`Boost::boost`); found, not fetched. |
| Qt6 | 6.x | Components `Core Gui GuiPrivate Quick Qml`. `GuiPrivate` needs the distro's private-dev package (see Troubleshooting). Dev host: 6.11.2. |
| GStreamer + GES | 1.x | `gstreamer-1.0`, `gstreamer-app/video/pbutils/gl-1.0`, `graphene-gobject-1.0`, `libges-1.0`. Dev host: 1.28.6. |
| OpenSSL | 3.x | **Optional, only for `GENESIS_UPDATER=ON`** (Ed25519 release-manifest verification); a default checkout does not need it. Dev host: 3.6.4. |

The runtime plugin sets (`gst-plugins-base`/`good`/`bad`/`libav` + the GL
elements) are needed at test time, not just build time: several suites drive a
real GES pipeline that instantiates `videotestsrc`, `vp8enc`, `webmmux`,
`glshader`, and friends.

### Arch (verified on this machine)

```sh
sudo pacman -S --needed base-devel cmake ninja pkg-config boost \
  qt6-base qt6-declarative \
  gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav \
  gst-editing-services graphene \
  openssl   # only needed for GENESIS_UPDATER=ON
```

Note: GES is the `gst-editing-services` package (not
`gstreamer-editing-services`), and it ships no `.pc` file — the build links it
raw; see Troubleshooting.

### Debian / Ubuntu (partly verified — see the Qt floor below)

**Qt >= 6.5 is required**, and this is the trap: Ubuntu 24.04 LTS ships Qt 6.4,
which has **no `Qt6GuiPrivate` component** (`Qt6GuiPrivateConfig.cmake` exists
nowhere in noble) and ships **no `<rhi/qrhi.h>` at all** — verified by
package-contents searches. So on stock 24.04 the Qt application **cannot be
built**, and `find_package(Qt6 ... GuiPrivate)` used to fail the whole configure.
It no longer does: a Qt older than 6.5 now degrades the build to the engine-free
host, the engine adapter and the non-Qt suites, with a warning naming what was
skipped. To build the app on 24.04, get a newer Qt (the Qt online installer, or
a distro/PPA that ships >= 6.5).

Also: `libgstreamer-gl1.0-dev` does **not** exist in noble — the `gst/gl/*`
headers ship inside `libgstreamer-plugins-base1.0-dev`.

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  cmake ninja-build pkg-config g++ libboost-dev \
  qt6-base-dev qt6-declarative-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-libav gstreamer1.0-gl libgstreamer-plugins-bad1.0-dev \
  libges-1.0-dev libgraphene-1.0-dev \
  libegl1-mesa-dev libgles2-mesa-dev libgl1-mesa-dri libegl1 libgles2-mesa
```

`libssl-dev` is only needed for `GENESIS_UPDATER=ON`; add it to the list above
when building that config.

`qt6-base-private-dev` supplies `Qt6::GuiPrivate`; the final EGL/GLES packages
are for the headless GL tests (see "Which suites need a display"). This is the
exact set the CI workflow (`./.github/workflows/ci.yml`) installs.

## Build and test

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Verified result on the dev host: 101/101 tests pass — 99 suites plus the
`format_check` and `qml_format_check` gates (see the CI file header for the
full invocation). Configure must run out-of-tree; `build/` is git-ignored.

### Building individual targets

```sh
cmake --build build --target genesis_app      # the Qt Quick GUI (output: build/bin/genesis-app)
cmake --build build --target genesis_cli      # the host CLI      (output: build/bin/genesis-cli)
cmake --build build --target core_tests       # any single test binary
```

`genesis_app` is the only target that links both Qt and GES; `genesis_cli` links
the GES engine adapter (for the `render` verb) plus the host core and CLI11.
Every verb except `render` runs without touching the engine, and none needs a
display.

### Formatting and linting

`.clang-format` (style) and `.clang-tidy` (checks) are the official coding
standard, and the durable rule is that **lint/format settings stay the
canonical, popular ones** — no hand-tuned deviation. The C++ style is the
**Qt-ecosystem style**: a faithful port of Qt's
canonical `_clang-format` (qt5.git) and Qt Creator's `.clang-format` — the most
common style for Qt6 projects (Qt, Qt Creator, KDE, and MLT all use it;
research-backed). Key settings: `BasedOnStyle: WebKit` with a 100-column limit,
pointer/reference types on the right (`Type *ptr`, `Type &ref`), Qt's brace
placement (attached control statements, broken functions/classes/structs), no
namespace indentation, and include order preserved (no `SortIncludes`). The
config is a modern spelling of Qt's file, adapted for the clang-format 19-22
option renames (the header comment in `.clang-format` lists each deviation).
Formatting is driven through CMake, and `format_check` is what CI enforces (see
`.github/workflows/ci.yml`):

```sh
cmake --build build --target format        # rewrite src/ + tests/ in place
ctest --test-dir build -R format_check     # fail if anything is unformatted
cmake --build build --target tidy          # manual, report-only clang-tidy pass
cmake --build build --target cppcheck      # manual, report-only cppcheck pass
```

Run `cmake --build build --target format` after every change; the `format_check`
suite must pass. Each target skips cleanly (with a configure warning) when its
tool is absent, matching the Qt/AI/gRPC degradation pattern.

**clang-tidy runs the canonical check set** from the most-used clang-tidy CI
action, `ZedThree/clang-tidy-review` (its `Checks` default, verbatim):
`-*,performance-*,readability-*,bugprone-*,clang-analyzer-*,cppcoreguidelines-*,mpi-*,misc-*`.
This is the same "keep it canonical" rule as clang-format: the popular default,
not a hand-picked narrow set. `tidy` is a manual, report-only pass that keys off
`compile_commands.json` (exported via `CMAKE_EXPORT_COMPILE_COMMANDS`); module
scanning is disabled (`CMAKE_CXX_SCAN_FOR_MODULES OFF`) so CMake 4.4 + GCC 16 do
not inject `-fmodules-ts`/`-fmodule-mapper`/`-fdeps-format` flags that
clang-tidy's frontend rejects as unknown. Findings are a baseline, not a list to
clear.

**clang-format must be pinned to 22.1.8.** clang-format's output is not stable
across versions — a different version reformats the same file differently, so
an unpinned formatter would make `format_check` pass or fail spuriously. The
dev host and CI both use 22.1.8 (CI pip-installs `clang-format==22.1.8`;
Ubuntu's apt ships clang-format-18, not 22). The tree was reformatted with
22.1.8. `clang-tidy` 22.1.8 is the matching linter; it reads
`compile_commands.json` (exported via `CMAKE_EXPORT_COMPILE_COMMANDS`), so it
needs a successful configure first.

**cppcheck** is the optional, report-only third tool (clang-format + clang-tidy
+ cppcheck). It runs its own preprocessor over `src/` with cppcheck's standard
categories (`--enable=warning,performance,portability`) and never fails the build
(`--error-exitcode=0`); it is not a gate and is not in ctest. Findings are a
baseline to inspect when touching risky code, not a list to clear. Coverage is
`src/` with `-I src` only — Qt/GStreamer system headers are not passed, so they
go unresolved (silent: the `missingInclude` category is not enabled) and
analysis inside third-party headers is skipped. Suppress a false positive on the
exact line with `// cppcheck-suppress <id>` (enabled by `--inline-suppr`), or
file/category-wide via `.cppcheck-suppressions` (one `--suppress`-style rule per
line). Like clang-format/clang-tidy, the target skips cleanly with a configure
warning when cppcheck is absent.

### QML formatting and linting

QML follows the same gated-CMake pattern. `.qmlformat.ini` (style) and
`.qmllint.ini` both use their Qt defaults (qmllint's default severities), and
are the official standard; the context
properties that QML reads from C++ are whitelisted in `.contextProperties.ini`
(18 properties, kept in sync with `AppShell.cpp`):

```sh
cmake --build build --target qml_format      # rewrite src/app/qml/ + panels in place
ctest --test-dir build -R qml_format_check   # fail if anything is unformatted
cmake --build build --target all_qmllint     # report-only qmllint pass
```

Run `cmake --build build --target qml_format` after every QML change; the
`qml_format_check` suite must pass. Both the target and the suite skip cleanly
(with a configure warning) when `qmlformat` is absent. `all_qmllint` is
auto-generated by `qt_add_qml_module` and is deliberately **not** a gate: it
emits a known set of pre-existing warnings — roughly 369 `unqualified-access`
(ids, model roles, and `Theme` references inside Repeater delegates) and 224
`missing-property` — that are not context-property mistakes. Do not whitelist
new ones in `.contextProperties.ini`.

**qmlformat must match the Qt the tree was formatted with (6.11).** Like
clang-format, qmlformat's output is not stable across Qt versions; a different
version would make `qml_format_check` pass or fail spuriously. The tree was
formatted with Qt 6.11 (qmllint/qmlformat 6.11.2).

### Trialing a candidate pack

`genesis-cli packs trial <dir>` vets a candidate Pack directory without
executing it: it loads and validates the manifest (reporting every problem),
then compiles the Pack's GLSL body headlessly on a fresh offscreen context.
Add `--validate-only` to stop after manifest validation. Nothing is installed
or trusted — a valid Pack is reported as "validated, not trusted" with the
trust/install path spelled out.

```sh
cmake --build build --target genesis_cli
build/bin/genesis-cli packs trial <dir> --validate-only
```

### Native extensions (SDK)

The native-extension SDK is host-side and engine-free. Author guide and C ABI:
[`docs/extensions.md`](extensions.md); the worked example is
[`examples/hello-extension/`](../examples/hello-extension/), a standalone
project with its own `CMakeLists.txt` (not part of the main build).

`genesis-cli extensions install --origin developer <dir>` installs into the
extension store, and `genesis-cli extensions list` shows what is installed.
Extensions arrive from three sources: a local **directory**, a **git** repo
(`extensions install --git <url> [--ref <ref>] [--subdir <subdir>]`), or a
**catalog** (`extensions catalog <url>` lists one, `extensions install
--catalog <url> <id>` installs an entry). A source never elevates trust: the
default origin is `user` (refused), so pass `--origin developer` or `curated`.

In the app, the same workflow lives in the **Extensions** dialog
(`src/app/qml/ExtensionsDialog.qml`): **Install extension…** copies a picked
directory, the **marketplace** section fetches a catalog URL and installs a
listed entry, and the **config profile** section exports the machine's settings
and installed set to a JSON file or imports one back
(`genesis-cli config export <file>` / `genesis-cli config import <file>
[--no-install]` are the CLI forms). See [`docs/extensions.md`](extensions.md)
§6-§7 for the full source, catalog, and profile rules.

The host-side suites (`install`, `sources`, `source_install`, `catalog`,
`config`, `consent`, `disabled_list`, `removed_list`, `seed`, `manager`,
`extension_host`, `extension_scan`) are hermetic: they scan a
temp store and `dlopen` C fixture libraries, so they need no GStreamer, GL, or
display. `command_codec` pins the
Command JSON that an extension's `edit.apply` bridge call carries, and the Qt
`extensions_controller` suite drives scan -> consent -> invoke headlessly (built
only when Qt >= 6.5 is present).

## AI runtimes (optional, default OFF)

The default build compiles `genesis_ai` but fetches no inference runtime, so a
fresh checkout stays dependency-free. Each runtime is a CMake option and every
one implies the `GENESIS_AI` master switch:

| Option | Runtime it pulls in | Enables |
|---|---|---|
| `GENESIS_AI_CAPTIONS` | whisper.cpp (pinned tag, fetched static) | `WhisperProvider` transcription |
| `GENESIS_AI_VISION` | ONNX Runtime (system first, else pinned prebuilt tarball) | RVM/IS-Net segmentation, Real-ESRGAN enhance, SlimSAM brush |
| `GENESIS_AI_TTS` | sherpa-onnx (pinned commit, fetched static; pulls onnxruntime + espeak-ng + piper-phonemize) | `SherpaTtsProvider` speech |

With a runtime OFF, its provider compiles to a disabled provider that reports
`disabled` at runtime instead of failing to link; the app links one `genesis_ai`
and asks it what it can do.

```sh
cmake -S . -B build-ai2 -G Ninja -DGENESIS_AI_VISION=ON   # vision verify config
cmake -S . -B build-ai3 -G Ninja -DGENESIS_AI_TTS=ON      # TTS verify config
cmake --build build-ai2
```

Each runtime adds its provider suite: `GENESIS_AI_CAPTIONS` adds
`whisper_provider`, `GENESIS_AI_TTS` adds `tts_provider`, and `GENESIS_AI_VISION`
adds `segmentation_provider` and `brush_provider`; without a model on disk those
suites skip cleanly.

**GPU acceleration (`GENESIS_AI_GPU`, default OFF).** `GENESIS_AI_GPU=ON` builds
whisper.cpp's CUDA kernels. It requires the CUDA toolkit: when
`find_package(CUDAToolkit)` finds it, `GGML_CUDA` is turned on and
`GENESIS_AI_GPU_ENABLED` is defined; without it **configure warns and whisper
stays on CPU** (the caption provider still reports `enabled`). The option implies
`GENESIS_AI_CAPTIONS`, so a toolkit-less configure is a safe way to check the
warning without building anything. ONNX Runtime execution providers are separate
and runtime-detected: `GENESIS_AI_VISION` uses CUDA when ORT reports a CUDA
device and retries on CPU otherwise (`src/ai/vision/ExecutionProvider.h`; the
`execution_provider` suite covers the selection).

```sh
cmake -S . -B build-aig -G Ninja -DGENESIS_AI_GPU=ON   # GPU verify config (configure-only without CUDA)
```

**sherpa-onnx GCC 16 patch.** The pinned sherpa-onnx commit carries four
`u8"\u20XX"` string literals that GCC 16 rejects under C++20+ (a `u8` literal is
`const char8_t[N]` and will not convert to the `const char*` the surrounding
`std::pair` wants). Configure applies `cmake/patches/sherpa-onnx-supertonic-u8.patch`
via `PATCH_COMMAND` to drop the `u8` prefix; the universal-character-name escapes
still encode UTF-8 bytes in the narrow literal, so the output is unchanged. Drop
the patch command once the pin moves past the offending literals.

## Self-update core (optional, default OFF)

The self-update core in `src/workspace/update/` is gated by `GENESIS_UPDATER`
(default **OFF**). A default checkout still compiles the sources but reports
`disabled`: the Ed25519 verifier resolves to `Unavailable` and every install is
refused before a byte is fetched. `GENESIS_UPDATER=ON` requires OpenSSL 3.x;
without it CMake warns and the updater stays disabled.

The core covers a signed release manifest, Ed25519 verification via OpenSSL EVP,
streamed download with a digest check, staged install, atomic swap, and
`confirm`/`rollback`. The app surface is **notify-only** — `Settings` -> `Updates`
(`UpdateController`, context property `updates`) checks the feed and tells the
user a newer version exists; it never applies. The apply path is deferred (the
decision record `docs/decisions/auto-update.md`). The running app version comes
from `kAppVersion` in `src/app/Version.h`, compiled from the
`GENESIS_APP_VERSION` define CMake passes the app target (the header fallback
must track `PROJECT_VERSION`). The embedded `kReleasePublicKey` is all zeros, so
even an enabled build verifies nothing until a release key is minted.

```sh
cmake -S . -B build-upd -G Ninja -DGENESIS_UPDATER=ON   # updater verify config
cmake --build build-upd
ctest --test-dir build-upd -R "updater" --output-on-failure
```

`updater` runs in the default build (a stub verifier drives the crypto-free
pipeline, so no OpenSSL is needed); `updater_crypto` (Ed25519 sign/verify, key
pair generated in-process, no key on disk) is added only when the option is ON
and OpenSSL is found.

## gRPC transport (optional, default OFF)

The gRPC server transport in `src/cli/GrpcServe.{h,cpp}` is gated by
`GENESIS_GRPC` (default **OFF**), so the default checkout neither fetches nor
links gRPC. It reuses the JSON-RPC codec of the stdio `api` verb and the
Unix-socket `serve` verb: every Call carries one JSON-RPC 2.0 request and returns
one reply. The CLI exposes it as `serve --grpc`:

```sh
build/bin/genesis-cli serve --grpc --grpc-addr 127.0.0.1:50051 --token <token>
```

`--grpc` serves over gRPC instead of the Unix socket, and requires a non-empty
`--token`; every call must present `authorization: Bearer <token>`. Without the
option the `--grpc` flags are not registered and the verb stays host-only.

gRPC is a **heavy dependency**: a cold `GENESIS_GRPC=ON` build takes tens of
minutes. Configure resolves it in two stages — `find_package(gRPC CONFIG)` first
(an installed package is used as-is), otherwise a pinned `FetchContent` of
`v1.82.0` with the six submodules a C++ build needs, built C++17 for that fetch
only. `GENESIS_GRPC_ENABLED` is defined only when gRPC actually resolves; the
`grpc_serve` suite is registered only then.

```sh
cmake -S . -B build-grpc -G Ninja -DGENESIS_GRPC=ON    # gRPC verify config
cmake --build build-grpc
ctest --test-dir build-grpc -R grpc --output-on-failure
```

## Which suites need a display

All **101 `ctest` tests are display-free** — 99 suites plus the `format_check`
and `qml_format_check` gates; `genesis_app` (not registered with ctest) is the
only target that opens a window and needs a real display. The 99 suites are
**83 non-Qt + 16 Qt**; run `ctest -N` for the live list. Representative groups:

- **Engine-free host** — the `genesis_host` / `genesis_ai` core: `core`,
  `project`, `document`, `command_codec`, `clip_timeline`, effects and packs,
  caches, the extension suites (`install`, `consent`, `disabled_list`,
  `removed_list`, `seed`, `manager`, `extension_host`, `extension_scan`), the
  AI gating suites (`whisper_gating`,
  `segmentation_gating`, ...), `perf`. No GL, no GStreamer, no display.
- **CLI/transport** — `cli`, `socket_serve`: link the engine adapter (for `render`)
  but drive host-only verbs and the JSON-RPC codec/transport, never a pipeline.
- **GES adapter** — `ges_builder`, `ges_session`, `prepare`, `probe`,
  `effect_application`, `export`, the proxy/reverse builders,
  `named_intermediate`: compile a host graph into a GES pipeline and drive it
  without presenting frames.
- **GL with a self-created context** — `frame_texture`, `audio_output`,
  `shader_chain`, `reveal_effect`, `clip_effect`, `lut_grade`, `scopes`:
  create their **own** EGL context via `gst_gl_display_egl_new()` and hand it
  across the GStreamer seam (the Phase 0 spike finding: GStreamer's GL elements
  do not make their own context). They need a working EGL *default display*, not
  a display server — on a headless machine Mesa's `surfaceless` platform with
  llvmpipe satisfies this (`EGL_PLATFORM=surfaceless`). `audio_output` also
  degrades gracefully with no audio device.
- **Qt-controller (16)** — `edit_controller`, `title_renderer`, the
  `*_controller` set including the four AI controllers: built only when
  Qt >= 6.5 with `Qt6::GuiPrivate` is present, run headless. The AI controllers
  use injected fakes, so they run in the default build too (runtime-gated cases
  skip).

Consequence for CI: `ctest` is safe to run headless; the app is not.

## Troubleshooting

These traps are recorded elsewhere; they are linked rather than duplicated.

- **GES ships no `.pc` file** (Arch): link `-lges-1.0` against the GStreamer
  flags; headers under `/usr/include/gstreamer-1.0/ges/`. See
  `docs/dependencies.md` §"Installed" notes. The build already does this
  (`CMakeLists.txt` links `ges-1.0` raw).
- **`Qt6::GuiPrivate` is required** for `<rhi/qrhi.h>`, and it only exists from
  **Qt 6.5**. On Arch it arrives with `qt6-base`; on Debian/Ubuntu the RHI
  headers are simply **not packaged in noble** (Qt 6.4), so a newer Qt must come
  from outside the distro. The build tolerates the absence by skipping the Qt
  targets with a warning rather than failing; see the Debian/Ubuntu note above.
- **glib/Qt `signals` macro conflict**: GStreamer's `gdbusintrospection.h` has a
  field named `signals`, which collides with Qt's `signals` keyword macro.
  Include the GStreamer/GES headers before the Qt headers (the app does this in
  `src/app/GenesisApp.cpp:11-12`).
- **`x264enc` is absent** (it lives in `gst-plugins-ugly`, not installed): use
  VP8/VP9, AV1, OpenH264, Theora, or NVENC for test fixtures and export. See
  `docs/dependencies.md` §"Phase 0 spike result".

## For users

End users do **not** build Genesis-0. They install a single package; the package
manager resolves Qt6, GStreamer/GES, and codec runtime libraries for them. The
small helper libraries (nlohmann/json, toml++, CLI11) are statically compiled in
and **never ship** as separate components. See `docs/dependencies.md`
§"Build-time vs runtime dependencies" and `docs/decisions/packaging.md`.
