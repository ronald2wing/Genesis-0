# Genesis-0 — Dependency Status and License Evidence

Status: evidence read-only from the dev host on 2026-09-28. Checked: 2026-09-28.

## Evidence commands

`pacman -Qi <pkg>`, `pacman -Qq`, `pkg-config --modversion <pkg>`, `g++ --version`,
`qmake6 --version`, `gst-inspect-1.0`, `ffmpeg -version`. All read-only.

## Installed

| Component | Version | Declared license (package metadata) | Evidence |
|---|---|---|---|
| Qt6 base | 6.11.2-3 | `GPL-3.0-only OR LGPL-3.0-only OR LicenseRef-Qt-Commercial OR Qt-GPL-exception-1.0` | `pacman -Qi qt6-base`, `pkg-config Qt6Core` |
| Qt6 declarative | 6.11.2-1 | same Qt license set | `pacman -Qi qt6-declarative`, `pkg-config Qt6Quick` |
| Qt6 multimedia | 6.11.2-1 | same Qt license set | `pacman -Qi qt6-multimedia`, `pkg-config Qt6Multimedia` |
| GNU C++ (`g++`) | 16.2.1 | (compiler) | `g++ --version` |
| qmake6 | 6.11.2 | (Qt build tool) | `qmake6 --version` |
| GStreamer core | 1.28.6-3 | `LGPL-2.1-or-later` | `pacman -Qi gstreamer`, `pkg-config gstreamer-1.0` |
| GStreamer base plugins | 1.28.6-3 | `LGPL-2.1-or-later` | `pacman -Qi gst-plugins-base`, `pkg-config gstreamer-1.0` |
| `gst-plugins-good` | 1.28.6-3 | `LGPL-2.1-or-later` | `pacman -Q gst-plugins-good` |
| `gst-plugins-bad` | 1.28.6-3 | `LGPL-2.1-or-later` | `pacman -Q gst-plugins-bad` |
| `gst-libav` | 1.28.6-3 | `LGPL-2.1-or-later` (linked against distro FFmpeg) | `pacman -Q gst-libav` |
| `gst-editing-services` (GES) | 1.28.6-3 | `LGPL-2.1-or-later` | `pacman -Q gst-editing-services`, `libges-1.0.so.0.2806.0` |
| `cmake` | 4.4.3-2 | (build tool) | `cmake --version` |
| `ninja` | 1.13.2-3 | (build tool) | `ninja --version` |
| FFmpeg | 2:9.0.1-4 | `GPL-3.0-only` | `pacman -Qi ffmpeg`, `ffmpeg -version` |
| libavcodec / libavformat | 63.1.101 | (FFmpeg, GPL-enabled build) | `pkg-config libavcodec`, `libavformat` |

Notes:

- Qt6 is used under its own terms (LGPLv3/module terms and/or commercial); our GPL-3.0-or-later
  license does not override them (`docs/decisions/licensing.md`).
- The dev-host FFmpeg was built with `--enable-gpl`, `--enable-libx264`, `--enable-libx265`; its
  obligations differ from an LGPL-only FFmpeg build.
- **GES ships no `.pc` file on Arch.** `pkg-config gstreamer-editing-services-1.0` fails even when
  installed. Compile against it with the GStreamer flags plus `-lges-1.0`; headers live at
  `/usr/include/gstreamer-1.0/ges/`. Verified: `gcc probe.c $(pkg-config --cflags --libs
  gstreamer-1.0) -lges-1.0` links and runs.
- **`gst-editing-services` is the correct Arch package name**, not `gstreamer-editing-services`.

## Build-time vs runtime dependencies

The table above mixes two classes; they matter differently for users.

- **Build-time only** — `cmake`, `ninja`, `g++`, `qmake6`. Never shipped; users never install these.
- **Runtime** — Qt6 libs, GStreamer libs, GES (`libges`), FFmpeg libs. These ship with the app and
  must be declared in package metadata. Users install one package; the package manager resolves the
  rest. Users do **not** run `pacman -S`/`apt install` by hand.
- **Codec caveat** — `gst-plugins-bad` and `gst-libav` carry patent-encumbered codecs (H.264 etc.).
  Some distros split these into separate repos, and they cannot be statically bundled freely. This
  constrains packaging the same way the GPL decision does; Flatpak is the path of least resistance.

### Small libraries are fetched, not installed (2026-09-29)

C++ has no `cargo`/`npm`, so helper libraries used to require a distro package each. That ended:
small third-party libraries are now declared with CMake `FetchContent` and a **pinned tag**, so a
fresh checkout needs only Qt6 + GStreamer/GES (plus a compiler, CMake, Ninja).

```cmake
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    FIND_PACKAGE_ARGS 3.11 NAMES nlohmann_json   # use a system copy if it satisfies the floor
    EXCLUDE_FROM_ALL
)
```

`FIND_PACKAGE_ARGS` means: prefer an installed package when it meets **our** version floor, otherwise
download and build it. Either way the version requirement is the project's, not the distro's — and
the forced-fetch path is verified (`cmake -DCMAKE_DISABLE_FIND_PACKAGE_nlohmann_json=ON` builds and
passes `ctest` 3/3 with no system package present).

Rule for new dependencies: **anything not needed at runtime goes through `FetchContent` with a pinned
tag.** Only Qt6 and GStreamer/GES remain system prerequisites, because they are large, engine-adjacent,
and (for GStreamer) plugin-registry-based.

Current state of the local machine: `nlohmann-json 3.12.0`, `catch2 3.16.0`, `gtest 1.18.0`,
`spdlog 1.17.0`, `fmt 12.2.0`, `cli11 2.7.2`, `boost 1.92.0` are installed, but with the policy above
they are conveniences for local iteration rather than requirements — a clean machine still builds.

### Gated AI runtimes (added 2026-10-02)

The AI subsystem is implemented but gated behind CMake options that all default **OFF**, so the
default build above is unaffected: no AI runtime is fetched, compiled, or linked unless an option
is turned on. The tables above and the runtime package metadata are unchanged for a default
checkout.

| Component | Version | Licence | Enabling gate |
|---|---|---|---|
| whisper.cpp | v1.9.4 (pinned tag) | MIT | `GENESIS_AI_CAPTIONS` |
| ONNX Runtime | 1.29.0 (system first, else pinned Linux x64 release tarball) | MIT | `GENESIS_AI_VISION` |
| sherpa-onnx | commit `040afe36` (pinned, built static) | Apache-2.0 | `GENESIS_AI_TTS` |

Notes:

- whisper.cpp and sherpa-onnx are fetched static (`BUILD_SHARED_LIBS OFF`) so `genesis_ai` links a
  `.a` and ships no runtime dependency. sherpa-onnx pulls onnxruntime, espeak-ng and
  piper-phonemize as subprojects.
- ONNX Runtime is discovered via `find_package` first; the fallback fetches the pinned prebuilt
  tarball (`libonnxruntime.so`, SONAME `libonnxruntime.so.1`), which must be discoverable at run
  time (rpath or `ldconfig`).
- **sherpa-onnx carries a local patch.** The pinned commit has four `u8"\u20XX"` string literals
  that GCC 16 rejects under C++20+ (`u8` is `const char8_t[N]`, which will not convert to the
  `const char*` the surrounding `std::pair` wants). Configure applies
  `cmake/patches/sherpa-onnx-supertonic-u8.patch` to drop the `u8` prefix; the escapes still encode
  UTF-8 bytes in the narrow literal, so the output is unchanged. Drop the patch once the pin moves
  past the offending literals. See `docs/building.md` §"AI runtimes".
- **No model weights ship.** Each model is downloaded after explicit consent, verified against a
  pinned SHA-256, and stored under the app's `AppDataLocation/models` — never the cache root
  (`docs/decisions/ai-provider.md`).
- **GPU acceleration needs the toolkit.** `GENESIS_AI_GPU` (default **OFF**, implies
  `GENESIS_AI_CAPTIONS`) builds whisper.cpp's CUDA kernels only when `find_package(CUDAToolkit)`
  finds the CUDA toolkit; without it CMake warns and whisper builds CPU-only. This is a build-time
  toolkit requirement, not a fetched dependency. ONNX Runtime execution providers are detected at
  runtime (CUDA when ORT reports it, else CPU), so the vision runtime never requires it at configure
  time.

### Gated updater dependency (added 2026-10-02)

The self-update core is implemented but gated behind `GENESIS_UPDATER`, default **OFF**, so the
**default build is unaffected**: a default checkout neither links nor needs it, and the updater
sources compile against a no-op verifier that reports `disabled`. OpenSSL is the only non-AI
optional dependency.

| Component | Version | Licence | Enabling gate |
|---|---|---|---|
| OpenSSL | 3.x (system) | Apache-2.0 | `GENESIS_UPDATER` |

- OpenSSL supplies the Ed25519 EVP primitives for release-manifest signature verification
  (`src/workspace/update/Signature.cpp`). Without it, CMake warns and the updater stays disabled even
  with the option ON: `GENESIS_UPDATER_ENABLED` is not defined, the embedded release key is all
  zeros, and every install is refused before a byte is fetched.
- The crypto-free `updater` suite runs in the default build; `updater_crypto` (Ed25519 sign/verify,
  seeded in-process, no key on disk) is added only when the option is ON and OpenSSL is found.

### Gated gRPC transport (added 2026-10-02)

The gRPC server transport is implemented but gated behind `GENESIS_GRPC`, default **OFF**, so the
**default build is unaffected**: a default checkout neither fetches nor links gRPC. Configure
first tries `find_package(gRPC CONFIG)` and, only if that fails, falls back to a pinned
`FetchContent` (`v1.82.0`). gRPC is a heavy dependency with a long cold build (tens of minutes,
including its own bundled Abseil/Protobuf/OpenSSL stack), so the fetch path is reserved for the
non-default verify config. `GENESIS_GRPC_ENABLED` is defined only when the option is ON and gRPC
actually resolves; the `grpc_serve` suite is registered only then. The default checkout does not
compile `GrpcServe.cpp` at all: the source joins the CLI only when the gate is enabled and resolved.

| Component | Version | Licence | Enabling gate |
|---|---|---|---|
| gRPC | v1.82.0 (system first, else pinned `FetchContent`) | Apache-2.0 | `GENESIS_GRPC` |

## Required missing

None. The Phase 0 toolchain is complete as of 2026-09-28.

## Not selected

- **Engine version lock**: GES remains tentative (T1); no fallback engine is planned
  (2026-10-03), so GES is the only planned engine and no version is locked. The Phase 0 spike
  (below) passed, which strengthens T1 but does not lock it. Mobile is out-of-repo.
- **AI provider/model**: no model weights are selected or installed by default. The AI runtimes are
  implemented but gated behind CMake options that default **OFF** — see §"Gated AI runtimes
  (added 2026-10-02)" above and `docs/decisions/ai-provider.md` §10. Individual models are
  downloaded only on explicit consent, after a runtime gate is enabled.
- ~~**Sandbox runtime** (web mini-app vs WASM): not chosen.~~ **Dropped (2026-10-03)** — the web
  mini-app/WASM sandbox option is removed; extension UI is native (Qt/QML panels from native
  extensions), see `docs/architecture.md` §10.
- **Packaging format**: not chosen. Flatpak is the leading candidate given GPL + codec constraints.

## Phase 0 spike result (2026-09-29)

The GES render-into-Qt-Quick spike **passed end to end**. Verdict: `docs/decisions/compositor-path.md`.
Evidence: `/tmp/opencode/ges-spike/` (engine half) and `spikes/ges-qt-render/` (Qt half).

1. **GES builds a timeline from our model.** Two clips added to a layer via
   `ges_layer_add_asset`, layer duration exactly `2000000000` ns. Rendered end-to-end to a file
   with `ges-launch-1.0` (2.03 s, 1280x720 VP8 output).
2. **Multi-pass shader chains work.** Two chained `glshader` elements (identity + invert) processed
   5 frames. Output caps: `video/x-raw(memory:GLMemory), format=RGBA, texture-target=2D` — frames
   stay **on the GPU as GL textures**, which is what Qt Quick consumes.
3. **GES output presents inside a Qt6 Quick window via RHI, zero CPU readback.** The
   `spikes/ges-qt-render/` half wraps Qt's RHI GL context for GStreamer
   (`gst_gl_context_new_wrapped`) and reaches PLAYING with `video/x-raw(memory:GLMemory),
   format=(string)RGBA, 640x360, texture-target=(string)2D`. The frame is wrapped as a `QRhiTexture`
   and committed to the scene graph; the run reaches EOS with no GL warnings under
   `GST_DEBUG="GST_GL*:4"`.

API constraints found (record, do not rediscover):

- **`ges_asset_request` (sync) returns null** for uncached assets. Use `ges_asset_request_async`
  with a `GMainLoop`. This is by design, not a bug.
- **`glshader` does not inject varyings/uniforms.** Each fragment shader must declare its own
  `precision mediump float; varying vec2 v_texcoord; uniform sampler2D tex;`. Omitting the
  precision qualifier fails compilation on this GLES-style GLSL.
- **`Qt6::GuiPrivate` is required** for `<rhi/qrhi.h>` on this distro (the QRhi headers are under the
  versioned private include dir). QRhi itself is public Qt API with limited compatibility guarantees.
- **`QQuickWindow::createTextureFromNativeObject` is removed in Qt 6.11.** Wrap the GL texture id
  with `QRhiTexture::createFrom({textureId, 0})` → `QQuickWindow::createTextureFromRhiTexture`
  instead.
- `x264enc` is **not** available (it lives in `gst-plugins-ugly`, not installed). VP8/VP9, AV1,
  OpenH264, Theora, and NVENC are available.

Still open from the spike: the reference editor's **extended-linear half-float working space** and
its **non-blocking scopes compute pass** were not exercised. `glshader` is fragment-only; a compute
pass would need `glcompute` or a custom element. This remains the top render-layer risk.

Settled since, by the multi-pass prototype (`spikes/gl-multipass/`, evidence in
`docs/decisions/effect-execution.md` §6): a **linear** `glshader` chain and a **spatial
fan-out/fan-in** (`tee` → `glshader` ×2 → `glvideomixer`) both work and stay GPU-resident; the
**named-intermediate** model does **not** work with stock elements — `glshader` binds one texture
per element and a second `sampler2D` silently aliases unit 0, and `Stage::shrink` is unrepresentable
because `glshader` is a base transform with identical in/out dimensions. **But it does work with a
custom element:** the follow-up spike (`spikes/gl-custom-element/`) built a `GstGLMixer` subclass
that binds N input textures into one shader, verified by pixel measurement (`combine` → yellow from
red + green, 30/30 frames `GLMemory`). See `docs/decisions/effect-execution.md` §6 — the cost is a
custom element to build, ship and maintain. Also reproduced here: without a forced
`caps=video/x-raw(memory:GLMemory),format=RGBA` capsfilter, `glcolorconvert` emits a zero-copy
`DMABuf` rather than a texture.

## Engine and codec test gate (do not assume)

- `gst-inspect-1.0` reports **123 plugins / 728 features** on the dev host.
- `gst-libav` is now installed, so `av*` elements are available. Do **not** assert that `gst-libav`
  is automatically LGPL-clean, nor that it provides `avenc_h264`: actual codec plugins, build flags,
  and licenses decide, and a `gst-libav` linked against the distro's GPL-enabled FFmpeg can change
  obligations.
- **Export codec/profile availability is a test gate** pending inspection of the installed plugins,
  their build flags, and licenses. `ffmpeg -encoders` on the host lists `libx264`, `libx265`, and
  hardware H.264/HEVC encoders, but that is the FFmpeg CLI, not a cleared editor export path.

## No package-size claims

Package sizes are not asserted here; they depend on distro build options and were not weighed.

## Known issues

- **Scopes readback: the adapter fix is in, and the scopes panel is wired.**
  `src/adapters/engine/ges/scopes/Scopes.cpp` used to create its own GL context and read back on it; where
  that context could not see a texture painted by the pipeline's context, every readback came back
  the correct size and wholly black (a CI runner under software GL — Xvfb + llvmpipe + X11 EGL —
  reported `readback red histogram: 76800 px, peak at bin 0`).

  The adapter now has a preferred `Scopes(GstGLContext*)` constructor that reads on the pipeline's
  own GL context — the one the session installs — so there is no cross-context share to come up
  empty. `tests/adapters/ScopesTests.cpp` builds with that constructor and its content assertions
  are unconditional again; the suite passes in both environments (headless EGL and software
  llvmpipe + surfaceless), each printing a non-zero peak (`peak at bin 255`).

  The pane is now the builtin `genesis.scopes` extension panel
  (`extensions/builtin/genesis.scopes/ScopesPanel.qml`), opened through the extension host rather
  than a shell QML file, and fed by the `scopes.snapshot` API. `src/app/AppShell.cpp` builds the
  adapter, injects its frame source (`ScopesController::setFrameSource`) and submits every
  presented frame to `read_back`, and serves `scopes.snapshot` from `ScopesController`. What
  remains is the app's constructor choice: it still builds the adapter from raw `GlHandles`
  (`Scopes(GlHandles)`, `Scopes.h:111`, documented as the alternative form used by the app wiring), so
  the migration to hand it the session's `GstGLContext*` is still due.

- **`gst-editing-services` ships no `.pc` file on Arch**, so GES is linked raw as `-lges-1.0`; see
  the installed-notes above. On Debian/Ubuntu the same raw link works because `libges-1.0-dev` puts
  `libges-1.0.so` on the default linker path.
