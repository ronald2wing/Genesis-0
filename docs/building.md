# Genesis-0 — Building

How to build and test Genesis-0 from a fresh checkout. Developers build; end users do not.

Status: verified on the dev host (Arch, Qt 6.11.2, GStreamer/GES 1.28.6, GCC 16, CMake 4.4, Ninja 1.13). Checked: 2026-09-29.

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

Verified result on the dev host: 116/116 tests pass — 113 suites plus the
`perf` benchmark and the `format_check` and `qml_format_check` gates (see the CI
file header for the full invocation). Configure must run out-of-tree; `build/`
is git-ignored.

### Perf benchmark budgets

The `perf` suite's hard @10000 gates are calibrated on the dev host; a CI
runner is ~10x slower (shared, throttled vCPU), so the same ceilings would
hard-fail a healthy CI run. The driver therefore scales the hard gates by a
**budget scale factor** (`tests/perf/BenchmarkTests.cpp`):

- default `1.0` (dev host);
- `8.0` when the `CI` env var is set (GitHub Actions sets it);
- an explicit `GENESIS_PERF_BUDGET_SCALE` (any positive value) wins over both.

The scale relaxes only the hard @10000 gates — the @100 warn-only fixtures
never fail the run, so they stay unscaled. The applied scale is logged in the
suite's output header. The per-scenario ceilings, their dev-host baselines and
the observed CI p50s live beside the `kBudgets` table and in the block comment
at the top of that file; the CI numbers (`frame_plan` ~54 ms, `undo` ~206 ms)
come from CI run 37238435635. At 8x the @10000 ceilings are 480 ms
(`frame_plan`), 640 ms (`undo`), 8 s (`file_open`) and 80 ms (`scrub`) — ~3x
headroom over the observed CI p50 for `undo`/`file_open`/`scrub` and ~9x for
`frame_plan`, so an order-of-magnitude regression still trips on both hosts.
If CI variance grows past that 3x headroom, raise `GENESIS_PERF_BUDGET_SCALE`
rather than re-tuning a per-scenario table off one observation.

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
common style for Qt6 projects (Qt, Qt Creator, and KDE all use it;
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
not a hand-picked narrow set. On top of that base, `.clang-tidy` carries a
documented tuning layer — three `-<check>` exclusions and one CheckOption for
checks that are known noise on this tree; the rationale and project norm for
each are commented in `.clang-tidy`, and the list is reproduced in the
"clang-tidy exclusions" subsection below. `tidy` is a manual, report-only pass
that keys off
`compile_commands.json` (exported via `CMAKE_EXPORT_COMPILE_COMMANDS`); module
scanning is disabled (`CMAKE_CXX_SCAN_FOR_MODULES OFF`) so CMake 4.4 + GCC 16 do
not inject `-fmodules-ts`/`-fmodule-mapper`/`-fdeps-format` flags that
clang-tidy's frontend rejects as unknown. Qt 6.11's `Qt6::Platform` target
injects `-mno-direct-extern-access` for GNU (clang spells the same knob
`-fno-direct-access-external-data`), so the `tidy` target strips that GNU flag
via `--removed-arg` before running. Findings are a baseline, not a list to
clear.

**clang-tidy exclusions.** The canonical category block produces ~22.7k findings
tree-wide; ~73% come from four checks that are noise here and are tuned in
`.clang-tidy` (each is commented there with its reason and the project norm it
protects):

- `misc-include-cleaner` — **excluded**. GStreamer/GES are umbrella-header
  libraries (`gst.h`, `pbutils.h`, the GES `*.h` headers), so "no header
  providing X is directly included" is the normal (and required) include shape,
  not a defect; hand-including GStreamer internals breaks on every upstream
  header move.
- `readability-identifier-length` — **excluded**. The tree's idiomatic short
  names (`id`, `ok`, `in`, `it`, `ec`, `c`, `a`/`b` comparison params, `j` loop
  index, `at`) are the domain's own words, which the 3-char minimum rejects
  wholesale; the per-kind minimums cannot separate them from real noise without
  an arbitrary ignore list.
- `cppcoreguidelines-pro-bounds-avoid-unchecked-container-access` — **excluded**.
  A blanket ban on `operator[]`; the tree indexes vectors/maps in loops and
  static lookup tables whose index is in-bounds by construction. LLVM and Google
  disable this check too.
- `misc-non-private-member-variables-in-classes` — **CheckOption**
  `IgnoreClassesWithAllMemberVariablesBeingPublic: true`, not an exclusion. The
  tree's value types (`Ease`, `Key`, `Animation`, the API request DTOs) are
  all-public-data structs ported from Rust that deliberately combine data with a
  small helper (`operator==`, `apply`, `tidy`). The option silences only classes
  whose members are *all* public, so a class that mixes public and private
  members is still flagged — exactly the encapsulation smell the check exists to
  catch.

**Re-baseline** (run the `tidy` target, or a representative ~20-file sample for
speed) and tally the check names; the goal is a top-category list you would act
on, not a clean sheet:

```sh
cmake --build build --target tidy > /tmp/tidy.txt 2>&1
grep -oE '\[[a-z0-9-]+\]$' /tmp/tidy.txt | sort | uniq -c | sort -rn
```

Only add another exclusion when a category is confirmed noise on this tree (not
merely numerous), and comment it in `.clang-tidy` with its reason and norm.

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
(20 properties, kept in sync with `AppShell.cpp`):

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

Accessibility is not lint-gated, so preserve it by hand: interactive controls
carry Qt Quick `Accessible` metadata. The design-system widgets (`Button`,
`ToggleButton`, `InputField`, `LabelledSlider`) set `Accessible.role` (plus
`name`/`value`/`checked`), and custom controls (timeline, transport, stroke
overlay) set `Accessible.role` and a description. `all_qmllint` emits no
`missing-role` warning, so a control reworked without its role degrades silently
for assistive technology.

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

## Dev tools

`build/bin/genesis-dev` is the agent-facing wrapper around the build, test, and
UI smoke gates:

```
build/bin/genesis-dev verify               # configure + build + flake-aware ctest + smoke (default full gate)
build/bin/genesis-dev verify --skip-smoke  # build + flake-aware ctest only
build/bin/genesis-dev smoke [--surfaces keyframes,shell]   # UI smoke gate
build/bin/genesis-dev suites [<files…>]    # suite -> sources map for targeted ctest -R
```

- **`verify`** is the default full gate: it configures and builds `build/`, runs
  ctest with flake classification (a flaky failure is retried and reported as
  flaky rather than fatal), then runs the smoke gate.
- **`smoke`** is the gate for UI changes. For each surface it spawns
  `genesis-app`, samples its RSS, and asserts **zero QML runtime messages** plus
  a bounded peak RSS (default 2048 MB, `--max-rss-mb`). The default surface set
  covers the main panes; `--surfaces` selects a comma-separated subset
  (`keyframes`, `shell`, `titles`, `scopes`, `extensions`, `settings`, `export`,
  …). Reports land in `build/ai-smoke/`: one PNG per surface plus `report.json`
  (per-surface spawn/exit/wall time, PNG byte size, peak RSS, QML lines).
  The app's no-arg launch builds a demo project from a temp media fixture
  (`/tmp/ges-spike/a.webm`, fallback `/tmp/opencode/clip.webm`); `smoke`
  generates that fixture itself (via `gst-launch-1.0`) when neither exists, so a
  cleaned temp dir cannot masquerade as a UI regression. A missing fixture is a
  gate setup failure (exit 2), not a per-surface failure.
- **`suites`** prints the map from suite name to source files, so you can pick a
  targeted `ctest --test-dir build -R <suite> --output-on-failure` without
  running all 118.
- **Limitation:** `smoke` opens surfaces against a **fresh, empty project**, so
  errors that require a loaded project plus a selection — e.g. playback-driven
  pane code — may not reproduce. Those paths still need the full `ctest` run or
  a manual load-project exercise.

## Translations

UI strings live in JSON catalogues under [`assets/i18n/`](../assets/i18n/),
one file per locale named `<locale>.json`. The app has no Qt `.ts`/`.qm`
translation pipeline yet: the `I18n` support object (`src/app/support/I18n.h`)
loads a catalogue at startup and QML resolves strings through the `i18n`
context property's `translate(key)` invokable (e.g.
`i18n.translate("start.open")`). Fifteen files ship today — **14 languages
(the English source plus 13 machine-drafted translations) plus 1
pseudo-locale (`en-XA`)**:

| File | Purpose |
|---|---|
| `assets/i18n/en.json` | The English source catalogue — the authoring reference. |
| `assets/i18n/de.json`, `es.json`, `fa.json`, `fr.json`, `hr.json`, `it.json`, `ja.json`, `ko.json`, `pt-BR.json`, `ru.json`, `tr.json`, `zh-Hans.json`, `zh-TW.json` | Machine-drafted, **unreviewed** translations (see "Reviewing a draft"). |
| `assets/i18n/en-XA.json` | The pseudo-locale, **non-production** (see below). |

### JSON format

Each file is a flat `key -> string` map. Keys are dotted identifiers that name
a screen and a purpose (`start.open`, `tray.quit`); the value is the
untranslated (or translated) string. Non-string values are ignored by the
loader, as is any top-level key beginning with `_`. The English file is the
source of truth: it defines the full key set, and every other locale is
expected to cover those keys.

A translation file may carry a top-level `_meta` object with loader-private
metadata — it is never part of the translatable catalogue. The machine-drafted
locales mark themselves unreviewed:

```json
{
    "_meta": {
        "status": "unreviewed-machine-draft",
        "source": "en",
        "generated": "2026-10-04"
    },
    "app.title": "Genesis-0",
    "start.open": "Open"
}
```

### Locale resolution

The active locale resolves in this order:

1. `GENESIS_LOCALE` — an environment-variable override (e.g.
   `GENESIS_LOCALE=de` or `GENESIS_LOCALE=en-XA`), read at startup;
2. `QLocale::system().name()` — the system locale Qt reports (`en_US`, `de_DE`,
   …);
3. `en` — the fallback.

The requested name is matched against the shipped files by folding case and
treating `-` and `_` as equivalent, so `en-US`, `en_US` and `en` all find
`en.json`. A requested name with a region (`de_DE`) falls back to the
language-level file (`de.json`) when only that ships. A locale whose file does
not exist degrades to English, never to failure.

### Fallback rules

For any `translate(key)`:

1. the active locale's value when present;
2. else the English value;
3. else the key itself.

A key with no translation anywhere is shown verbatim (visible, not fatal), so
an untranslated string can never crash startup or blank a control.

`availableLocales()` lists every discovered locale (filename minus `.json`,
sorted) and `currentLocale()` reports the locale actually selected after
fallback.

### Adding a locale

1. Copy `assets/i18n/en.json` to `assets/i18n/<locale>.json` (BCP-47 name such
   as `de`, `fr`, or `zh-Hans`).
2. Translate every value; keep the keys byte-identical. Do not rename, add, or
   drop keys in a translation file — keys are the contract.
3. Leave product names (`app.title`) untranslated.
4. Run the build and the `i18n` suite; verify with `GENESIS_LOCALE=<locale>
   build/bin/genesis-app` that the start screen and tray strings render.
5. Update the locale count in this section.

Review expectations: strings are short UI labels, so stay concise, keep
capitalisation consistent with the English original, and avoid inserting
format specifiers or markup. A translation PR should touch only its own
`<locale>.json` (never `en.json` or unrelated files).

### Reviewing a draft

The 13 machine-drafted locales ship as clearly-unreviewed catalogues so they can
be picked up by a translator without implying sign-off. Each carries a `_meta`
object whose `status` is `unreviewed-machine-draft` — a human review must turn
it into a reviewed locale:

1. Take ownership of `assets/i18n/<locale>.json`; open it beside `en.json` and
   correct the strings (terminology, gender, formality, capitalisation).
2. Change `_meta.status` from `unreviewed-machine-draft` to `reviewed` (and, if
   useful, add a `reviewer` field). Remove `_meta` entirely only if the project
   stops tracking review state.
3. Keep the key set byte-identical to `en.json`; the `i18n` suite fails a locale
   that drops a key or adds an extra translatable key.
4. Do not translate product names (`app.title`, `Genesis-0`); keep any future
   format specifiers (`%1`, `{0}`) exactly as in `en.json`.

Until a draft is reviewed, the `i18n` suite only pins that its keys match the
English set and that `_meta.status` is the unreviewed marker — it does not
assert translation quality.

### Pseudo-locale (`en-XA`)

`en-XA.json` is a generated, clearly non-production variant of the English
catalogue used to catch hardcoded strings and to exercise expansion/fallback in
the `i18n` suite. It is produced by a deterministic transform of `en.json`:
wrap each value in `[` … `]`, accent every ASCII vowel (`a→ä`, `e→é`, `i→ï`,
`o→ö`, `u→ü`, upper-case likewise), and append `  ~~~` padding to simulate
longer translated text. Regenerate it by re-applying that transform to
`en.json`; it is never authored by hand and is never selected automatically
(only via `GENESIS_LOCALE=en-XA`).

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

**GPU acceleration (`GENESIS_AI_GPU`, default OFF).** Builds whisper.cpp's CUDA
kernels when the CUDA toolkit resolves; otherwise configure warns and whisper
stays on CPU. Dependency, gate, and provider-selection detail are in
[`dependencies.md`](dependencies.md); the verify config is:

```sh
cmake -S . -B build-aig -G Ninja -DGENESIS_AI_GPU=ON   # GPU verify config (configure-only without CUDA)
```

**sherpa-onnx GCC 16 patch.** `cmake/patches/sherpa-onnx-supertonic-u8.patch`
drops four `u8"\u20XX"` literals GCC 16 rejects; see
[`dependencies.md`](dependencies.md) for the rationale.

## Self-update core (optional, default OFF)

The self-update core is gated by `GENESIS_UPDATER` (default OFF); a default
checkout compiles it under a no-op verifier that reports `disabled`. The
signature/digest/stage/swap detail and the OpenSSL gate are in
[`dependencies.md`](dependencies.md) and
[`decisions/auto-update.md`](decisions/auto-update.md). The verify config:

```sh
cmake -S . -B build-upd -G Ninja -DGENESIS_UPDATER=ON   # updater verify config
cmake --build build-upd
ctest --test-dir build-upd -R "updater|apply" --output-on-failure
```

`updater` and `apply` run in the default build (stub verifier + fake spawner);
`updater_crypto` and `catalog_crypto` are added only with the option ON and
OpenSSL found.

The running app version is `kAppVersion` in `src/app/Version.h`, compiled from
the `GENESIS_APP_VERSION` define CMake passes the app target; the header's
fallback must track `PROJECT_VERSION`.

## Processed-media cache

The app's generated caches (waveforms, filmstrips, posters, reverses, proxies,
masks, title PNGs) live under three roots — the flat cache root plus `masks/`
and `titles/` — and are bounded by the host sweep in
`src/workspace/CacheSweep.{h,cpp}` (evict oldest-first to a cap; source assets
and extension packs are never touched). Settings → Storage drives it through the
`cache` context property (`CacheController`): `cache.refreshSize()`,
`cache.sweep()` and `cache.clearCache()`; the cap persists in `cache.json` and
defaults to 10 GiB. Why this is a disk cap and not a RAM-derived budget (the reference `host`
crate's `memory.rs` in-RAM reader pool, not adopted): see
`docs/decisions/cache-and-memory-budgeting.md`.

### Drag and drop (QML)

The `MediaBin.qml` grid is a `DropArea` for OS file drops routed to
`mediaBin.importFiles(urls)` (one batch, one undo step), and each bin card is a
`Drag` with a private mime key that only `TimelineStrip.qml`'s `DropArea`
accepts, placing a clip via `edit.addClipAt(mediaId, trackId, seconds)`; a clip
body or edge drags through `edit.beginMove`/`updateMove`/`endMove`.

## Single instance and project claim

A second launch of the app forwards its project path to the running instance
and exits, via the `QLocalServer`/`QLocalSocket` pair in
`src/app/support/SingleInstanceGuard.{h,cpp}`. Opening a project takes an advisory
`flock` on `<root>/.genesis.lock` (`src/workspace/ProjectClaim.{h,cpp}`), so a
second process is refused with "the project is already open in another process"
and a crashed holder's lock frees itself on the next open.

## Logging

The app writes one run log per launch into `<app data>/logs/`, named
`genesis-YYYYMMDD-HHMMSS.log`, through the single `app_log` sink in
`src/app/support/AppLog.{h,cpp}`. Every line goes to stderr too, so a run from
a terminal reads exactly as it always did; a packaged run (no terminal) still
has the file. The folder keeps the newest 10 runs and each file stops growing at
16 MiB (`KEEP`/`CAP` in `src/workspace/Logs.h`) — a runaway loop cannot fill the
disk. That bound is the on-disk budget, distinct from the (unadopted) RAM probe:
see `docs/decisions/cache-and-memory-budgeting.md`.

### Levels

Four levels gate what is written, ascending: `error` < `warn` < `info` <
`debug`. A message is written when its level does not exceed the current one,
which defaults to `info`. `app_log(...)` writes at `info`; the leveled
`app_log_at(level, ...)` picks any of the four. `debug` lines compile in but
write nothing unless the level is `debug`. In the run log a non-`info` line
carries a `[error] ` / `[warn] ` / `[debug] ` prefix (so `grep '\[error\]'`
finds real problems); `info` lines keep the exact format they always had.

### Choosing the level

`GENESIS_LOG` in the environment sets the level once at startup, or the
`--log-level error|warn|info|debug` flag overrides it:

```sh
GENESIS_LOG=debug build/bin/genesis-app
build/bin/genesis-app --log-level warn
```

The name is case-insensitive and whitespace is ignored. An unrecognised value
is reported (a `[warn]` line on stderr) and the level becomes `info`; an
unset or empty variable leaves the default.

### Crash log

`src/app/support/CrashHandler.{h,cpp}` installs a `std::set_terminate` handler and, on
Linux/BSD/macOS, SIGSEGV/SIGABRT handlers. A crash appends a
`genesis crash: ...` reason line and a backtrace to stderr and to the current
run log, so a packaged app that "just disappeared" still leaves a trail. The
backtrace uses glibc's `backtrace()`/`backtrace_symbols_fd()` — it resolves
symbols only when the binary has them (a stripped release shows addresses,
which `addr2line` can still map), and it is deliberately limited to the
async-signal-safe calls (`open`/`write`/`close` plus the execinfo pair) so the
handler cannot recurse or deadlock. On other platforms the terminate handler
writes the reason to stderr alone.

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

### Server path confinement

Both remote surfaces (`serve` and `serve --grpc`) confine **writes**, unlike the
in-process `api` verb (the process's own owner at a terminal, which stays
permissive). The policy lives in `src/api/PathPolicy.h` (`PathPolicy` +
`check_request_paths`) and is enforced in `handle_jsonrpc_line`
(`src/cli/JsonRpc.cpp`) after decode and before dispatch, so a refused path is
never routed to a handler.

The default posture is **home + working directory**: a `project.save`, a
`project.close` with `save`, and an `export.run` `output` must resolve under one
of those roots (or under an operator-named root). Each `--allow-root <dir>`
(repeatable) widens the allowed set; name `/`
to allow anywhere:

```sh
build/bin/genesis-cli serve --allow-root /srv/projects --allow-root /srv/exports
build/bin/genesis-cli serve --grpc --token <token> --allow-root /srv/projects
```

Resolution refuses any `..` component and follows symlinks
(`std::filesystem::weakly_canonical`), so a path that escapes the roots on
paper or through a link is refused with the `Refused` code (`-32003`) and a
message naming the path. Reads (`project.open`, `media.probe`, `media.import`)
are **not** confined: a server may read media anywhere its owner can, matching
the reference editor's host crate, but cannot be asked to write outside its roots.

### Events stream

The gRPC service also exposes a **server-push `Events` RPC**, a transport-level
notification stream (not an extension API method). A client subscribes with the
same `authorization: Bearer <token>` metadata as `Call`, optionally filtered to
one topic; each `Event` carries one JSON-RPC 2.0 **notification** (no `id`),
spelled by the same codec envelope as a Call reply:

```json
{ "jsonrpc": "2.0", "method": "project.changed", "params": {} }
```

Topics emitted today:

- `project.changed` — after every successful Call whose method mutates project
  state (`project.create`, `project.open`, `project.close`, `project.save`,
  `project.setVideo`, `edit.apply`, `edit.undo`, `edit.redo`, `media.import`).
  Reads and the extension/config/AI verbs publish nothing.
- `serve.shutdown` — just before the server exits (a `shutdown` request,
  SIGINT, or SIGTERM), delivered to every subscriber so a client sees a clean
  stream end rather than a mid-flight cancellation.

The server fans events out through a bounded per-subscriber queue (64 deep,
drop-oldest), so a slow client cannot stall the broadcaster; a client that
stops reading just loses the oldest notifications. There is no heartbeat — the
stream is silent between events, and a vanished client is noticed on the next
write or an idle poll.

## Which suites need a display

All **116 `ctest` tests are display-free** — 113 suites plus the `perf`
benchmark and the `format_check` and `qml_format_check` gates; `genesis_app` (not
registered with ctest) is the only target that opens a window and needs a real
display. The 113 suites are **92 non-Qt + 21 Qt**; run `ctest -N` for the live
list. Representative groups:

- **Engine-free host** — the `genesis_host` / `genesis_ai` core: `core`,
  `project`, `document`, `command_codec`, `clip_timeline`, effects and packs,
  caches, the extension suites (`install`, `consent`, `disabled_list`,
  `removed_list`, `seed`, `builtin_root`, `manager`, `extension_host`,
  `extension_scan`), the asset locator (`asset_paths`), the
  AI gating suites (`whisper_gating`,
  `segmentation_gating`, ...), `perf`. No GL, no GStreamer, no display.
- **CLI/transport** — `cli`, `socket_serve`: link the engine adapter (for `render`)
  but drive host-only verbs and the JSON-RPC codec/transport, never a pipeline.
- **GES adapter** — `ges_builder`, `ges_session`, `prepare`, `probe`,
  `decode_policy`, `effect_application`, `export`, `waveform`, the proxy/reverse
  builders, `named_intermediate`: compile a host graph into a GES pipeline and
  drive it without presenting frames.
- **GL with a self-created context** — `frame_texture`, `audio_output`,
  `shader_chain`, `reveal_effect`, `clip_effect`, `lut_grade`, `scopes`:
  create their **own** EGL context via `gst_gl_display_egl_new()` and hand it
  across the GStreamer seam (the Phase 0 spike finding: GStreamer's GL elements
  do not make their own context). They need a working EGL *default display*, not
  a display server — on a headless machine Mesa's `surfaceless` platform with
  llvmpipe satisfies this (`EGL_PLATFORM=surfaceless`). `audio_output` also
  degrades gracefully with no audio device.
- **Qt-controller (21)** — `edit_controller`, `title_renderer`, `app_settings`,
  the `*_controller` set including the four AI controllers and `cache_controller`,
  and the `i18n`
  catalogue suite: built only when Qt >= 6.5 with `Qt6::GuiPrivate` is present,
  run headless. The AI controllers use injected fakes, so they run in the
  default build too (runtime-gated cases skip).

Consequence for CI: `ctest` is safe to run headless; the app is not.

## Hardware decode policy

The decode preference (`Auto`/`Software`/`Hardware`, the session's
`DecodePreference`) is applied at startup and on change by
`src/adapters/engine/ges/export/DecodePolicy.{h,cpp}`, which tilts GStreamer
plugin-feature ranks rather than naming a codec. Hardware decoders are the
factories whose klass carries the `"/Hardware"` suffix (the same criterion the
probe's `hardware_decode` reads, so the policy and the status surface agree).
The plan promotes each platform's decoder list one notch above the software
default (`GST_RANK_PRIMARY + 1`); `Software` demotes every ranked hardware
decoder to `GST_RANK_NONE` (the escape hatch); `Hardware` also demotes ranked
software decoders to `GST_RANK_SECONDARY`. A distro-disabled decoder (rank
`NONE`) is never resurrected.

The promoted element names are per-platform:

- **Linux** — exact: `va`, `vah264dec`, `vah265dec`, `vaapidecodebin`.
- **macOS** — exact: `vtdec`.
- **Windows** — prefixes: `d3d11`, `mf`.
- **all platforms** — the `nv` prefix (NVIDIA decoders, where present).

The `decode_policy` suite covers the pure `preference x platform x availability`
matrix over a fake registry plus on-host promote/demote/restore round-trips
against the real registry, and pins that the `Hardware` answer agrees with the
probe's `hardware_decode`. **Untested on this host:** the macOS and Windows
element names are exercised only through the pure matrix (`plan_decode_policy`
passes a platform explicitly); the on-host half runs the Linux lists. Verify
`vtdec`/`d3d11`/`mf` on real hardware before relying on them there.

## Hybrid graphics (PRIME)

On a hybrid Intel+NVIDIA laptop (PRIME render offload), the GL context the
probe reads back can land on the integrated GPU even though the NVIDIA driver
is loaded and its GStreamer elements (`nvh264dec`, `nvh265enc`, …) are present
— the status line then shows a Mesa/Intel renderer while encode still picks
`nvh265enc`. The probe reports the two facts separately: `renderer` is whichever
GPU the GL context chose, and `nvidia_available` / `nvidia_elements` report
NVIDIA presence via element factories and the `/dev/nvidia*` device nodes.

When the renderer is non-NVIDIA but NVIDIA is present, the startup log and the
GPU indicator's tooltip (the pill in the transport strip) print:

```
NVIDIA available — GL rendering on '<renderer>'; launch with `__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia` to render on NVIDIA
```

Launch with `__NV_PRIME_RENDER_OFFLOAD=1` to run the GL context on the NVIDIA
GPU; `__GLX_VENDOR_LIBRARY_NAME=nvidia` selects the NVIDIA GLX library and is
only needed on the GLX path. For EGL and NVENC, `__NV_PRIME_RENDER_OFFLOAD=1`
alone covers the offload.

## CI (GitHub Actions)

The workflow (`.github/workflows/ci.yml`) runs the display-free suites with
`ctest -j4` — deliberately not higher — because the heavy GL/GStreamer suites
each create their own llvmpipe EGL context and flake at high parallelism
(spurious EGL/pipeline timeouts were observed at `-j12`). On top of the main
build-and-test job it adds: a matrix of gated-config jobs (`-DGENESIS_AI_VISION=ON`,
`-DGENESIS_AI_TTS=ON`, `-DGENESIS_UPDATER=ON`) that build and run each
subsystem's suites (the model-backed providers skip without a model on disk,
and ai3 is the heavy leg — sherpa-onnx builds ONNX Runtime from source); a gRPC
job (`-DGENESIS_GRPC=ON`) that is configure + build-only and non-blocking,
because the vendored gRPC fetch is heavy; and a non-blocking diff-only
`clang-tidy` job over the changed C++ translation units. The offscreen
screenshot smoke (`QT_QPA_PLATFORM=offscreen genesis-app --screenshot …`) is
guarded on the app binary and is therefore dormant on `ubuntu-latest` (Qt 6.4
skips the app); it becomes live on a Qt >= 6.5 runner such as the dev host.

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

## Packaging

CMake installs the runnable artifacts and shipped data into a GNU-style prefix
layout, and CPack wraps them into a tarball (and, where the tools exist, a
DEB/RPM). Verified on the dev host; see `docs/decisions/packaging.md` for the
still-undecided distribution *format* (Flatpak is the leading candidate) — this
section covers the in-tree install/pack rules, not a final format.

### Install layout

`cmake --install build --prefix <prefix>` lays out:

```
<prefix>/bin/genesis-app            # the Qt Quick GUI
<prefix>/bin/genesis-cli            # the host CLI
<prefix>/share/genesis-0/assets/        # i18n/, luts/, titles/
<prefix>/share/genesis-0/extensions/builtin/  # the six builtin extensions
<prefix>/share/genesis-0/qml/            # the app's QML scene
```

Nothing build- or test-only is installed: no static libraries, headers, test
binaries, or `cmake/` scripts. The app's `genesis_app` install rule is guarded on
`GENESIS_CAN_BUILD_QT_TARGETS`, so a Qt < 6.5 configure (which skips the app)
installs only the CLI.

### Installed-path resolution

`AssetPaths::asset_root()` and `extensions::builtin_root()` fall back to
`<bindir>/../share/genesis-0/...` when the source-tree dev locations are absent,
so an installed binary finds its assets and builtin extensions without the
source tree:

- `asset_root()`: env `GENESIS_ASSET_DIR` → compile-time `GENESIS_ASSET_DIR` →
  walk up from cwd → `<bindir>/../share/genesis-0/assets` → walk up from the
  executable. (`src/workspace/AssetPaths.{h,cpp}`, suite `asset_paths`.)
- `builtin_root()`: compile-time `GENESIS_BUILTIN_EXTENSIONS_DIR` → beside the
  executable → `<bindir>/../share/genesis-0/extensions/builtin`.
  (`src/extensions/Seed.{h,cpp}`, suite `builtin_root`.)

**Known gap:** the app's QML scene is still loaded from the compile-time
`GENESIS_QML_DIR` (`src/app/AppShell.cpp`), which points at the source tree.
The QML files *are* installed to `share/genesis-0/qml/`, but the runtime
relocation of `GENESIS_QML_DIR` (matching the asset/builtin fallback) is not yet
done — a packaged app on a machine without the source tree will not find its
scene. This is a follow-up, not covered here.

### CPack

`include(CPack)` is on in the default configure. `CPACK_GENERATOR` is `TGZ`
always; the DEB and RPM generators are configured but added only when
`dpkg-deb` / `rpmbuild` are found at configure time (they are **absent on this
Arch dev host**, so the default build produces only the `.tar.gz`). Dependency
fields mirror `docs/dependencies.md` §"Build-time vs runtime dependencies" (Qt
6.5+, the GStreamer/GES and codec runtimes) and are approximate, per-distro
starting points. Windows (WIX/MSI) and macOS (DragNDrop/dmg) are the intended
installers on those platforms but are configured-but-unbuilt here.

```sh
cmake -S . -B build -G Ninja
cmake --build build
cmake --install build --prefix /tmp/genesis-pkg          # stage the prefix
(cd build && cpack -G TGZ)                               # build/genesis-0-<ver>-Linux-<arch>.tar.gz
```

### What's untested

- **DEB/RPM generation** — `dpkg-deb` and `rpmbuild` are absent on this Arch
  host, so the DEB/RPM dependency fields are written but never exercised. The
  tarball is verified (binaries + assets + builtin extensions).
- **Installed-app QML loading** — the `GENESIS_QML_DIR` relocation gap above.
- **WIX/DragNDrop** — no Windows/macOS host to build the installers.
- **Running the installed binary** — `genesis_app` needs a display, so the smoke
  here is layout/tarball inspection, not a launch.

## For users

End users do **not** build Genesis-0. They install a single package; the package
manager resolves Qt6, GStreamer/GES, and codec runtime libraries for them. The
small helper libraries (nlohmann/json, toml++, CLI11) are statically compiled in
and **never ship** as separate components. See `docs/dependencies.md`
§"Build-time vs runtime dependencies" and `docs/decisions/packaging.md`.
