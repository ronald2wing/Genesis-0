# Genesis-0

Status: **implemented**. The host core (C++23) is built as a Qt6 Quick/QML desktop application with a
GStreamer/GES engine adapter, using CMake + Ninja:

```sh
cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build
```

99 CTest suites pass (83 non-Qt + 16 Qt). Checked: 2026-10-03.

Genesis-0 is a modern video editor *inspired by* CapCut-class workflows (no CapCut branding or assets
are copied), targeting Windows, macOS, Linux, Android, and iOS.

## Current docs

- [`docs/architecture.md`](docs/architecture.md) — scope, decision register, engine strategy, UI and
  Extension/marketplace architecture. Design record.
- [`docs/implementation-plan.md`](docs/implementation-plan.md) — ordered phases, gates, acceptance
  tests. Historical planning record.
- [`docs/decisions/licensing.md`](docs/decisions/licensing.md) — licensing decision register
  (first-party GPL-3.0-or-later).
- [`docs/decisions/engine-choice.md`](docs/decisions/engine-choice.md) — engine decision (GES
  primary; MLT desktop-only fallback; native mobile adapters).
- [`docs/dependencies.md`](docs/dependencies.md) — installed / required-missing / not-selected
  dependency status with license/version evidence.

## Core timeline

`src/core/ClipTimeline.h` / `ClipTimeline.cpp` (portable C++23, no Qt) and
`tests/ClipTimelineTests.cpp` are implemented and wired as the `clip_timeline` CTest suite.

## Build status

The host core, the GStreamer/GES engine adapter, and the Qt6 Quick desktop application are present
and build against the installed toolchain (Arch, Qt 6.11.2, GStreamer/GES 1.28.6, GCC 16, CMake 4.4,
Ninja 1.13). All 99 CTest suites pass. Build commands are in
[`docs/building.md`](docs/building.md); dependency evidence is in
[`docs/dependencies.md`](docs/dependencies.md).

## License

First-party Genesis-0 code is **GPL-3.0-or-later** (full text: [`LICENSE`](LICENSE)). Third-party
dependencies keep their own licenses, which GPL-3.0-or-later does not override. The decision record
is in [`docs/decisions/licensing.md`](docs/decisions/licensing.md); dependency license/version
evidence is in [`docs/dependencies.md`](docs/dependencies.md).

No screenshots or images are included.
