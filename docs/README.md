# Genesis-0 — Documentation

This is the documentation for Genesis-0, a desktop-only (Windows/macOS/Linux) C++23 video editor
built on Qt6 Quick/QML + GStreamer/GES.

**Disclaimer.** No release, performance, or compliance claim is made anywhere in these documents.
Checked dates are recorded in each doc where relevant.

## Index

**Living docs (current design)**
- [architecture.md](architecture.md) — system map, decision register, engine strategy, UI architecture
- [building.md](building.md) — build, test, and run
- [dependencies.md](dependencies.md) — deps, licences, and version evidence
- [extensions.md](extensions.md) — Extension SDK author guide

**Decisions (ADRs)**
- [decisions/](decisions/) — 13 architecture decision records

**History (executed plans)**
- [history/rewrite.md](history/rewrite.md) — rewrite plan + command/undo layer (executed)
- [history/implementation.md](history/implementation.md) — phased implementation plan (executed)
- [history/unified-content-system.md](history/unified-content-system.md) — content-system redesign plan (executed)

**Proposals (open designs)**
- [proposals/extension-modification.md](proposals/extension-modification.md) — extension modification
- [proposals/level-3-replace-behavior.md](proposals/level-3-replace-behavior.md) — level-3 replace behaviour