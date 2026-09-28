# Genesis-0

**Anyone can make movies.**

Genesis-0 is a desktop video editor built so that the majority of people — not
just professionals — can make films. The hard parts (codecs, timelines,
effects, audio) are handled for you; the editing stays in your hands.

It is a C++23 Qt6 Quick/QML application over a GStreamer/GES engine adapter,
targeting Windows, macOS, and Linux (desktop-only). The host core and the CLI
are engine-free and Qt-free; the extension system (effect packs, titles,
panels, dialogs, native extensions) is the one unit everything user-installable
ships as.

## Build

```sh
# One-time: bootstrap vcpkg and export VCPKG_ROOT (see docs/building.md).
cmake --preset vcpkg
cmake --build build
ctest --test-dir build --output-on-failure
```

Outputs are `build/bin/genesis-app` (the editor) and `build/bin/genesis-cli`
(the headless CLI). `ctest -N` is the authoritative test count; every suite is
display-free. Prerequisites, distro package lists, gated configs (AI, gRPC,
updater) and troubleshooting are in [`docs/building.md`](docs/building.md).

## Documentation

- [`docs/README.md`](docs/README.md) — start here: the orientation index.
- [`docs/architecture.md`](docs/architecture.md) — system map: feature
  surfaces, decision register, engine strategy, UI and extension architecture.
- [`docs/building.md`](docs/building.md) — build, test, and run.
- [`docs/extensions.md`](docs/extensions.md) — Extension SDK author guide.
- [`docs/dependencies.md`](docs/dependencies.md) — dependency and license
  evidence.
- [`AGENTS.md`](AGENTS.md) — repo-specific instructions for coding agents,
  including the `genesis-dev` verify/smoke tooling.

## License

First-party Genesis-0 code is **GPL-3.0-or-later** (full text:
[`LICENSE`](LICENSE)). Third-party dependencies keep their own licenses, which
GPL-3.0-or-later does not override. The decision record is in
[`docs/decisions/licensing.md`](docs/decisions/licensing.md).
