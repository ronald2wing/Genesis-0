# Genesis-0 — Packaging Position

Status: **position recorded / not a decision**. The packaging *format* is still
not selected (`docs/dependencies.md` §"Not selected"); this document records the
constraints any choice must satisfy and what remains undecided. Checked:
2026-09-29. This is not legal advice and no compliance claim is made.

Companion evidence: `docs/decisions/licensing.md` (the governing constraint),
`docs/dependencies.md` §"Build-time vs runtime dependencies" and §"Not selected",
`docs/rewrite-plan.md` Phases 8-9.

## Constraints

1. **GPL-3.0-or-later.** First-party code and incorporated Concat code are
   GPL-3.0 (`docs/decisions/licensing.md`). Consequences that bind packaging
   — do not restate, cite: linked/combined works must be GPL-3.0, so a
   closed-source/paid Extension marketplace is dead and only GPL-compatible
   Extensions can ship in a distribution (a plugin/linking exception may be
   added later). No network-source obligation applies.
2. **Codec patents.** `gst-plugins-bad` and `gst-libav` carry patent-encumbered
   codecs (H.264 etc.); some distros split them into separate repos, and they
   cannot be statically bundled freely. This constrains packaging the same way
   the GPL decision does, and **Flatpak is the path of least resistance**
   (`docs/dependencies.md` §"Build-time vs runtime dependencies").
3. **App stores are a hard gate.** GPL copyleft is widely considered
   incompatible with Apple App Store and Google Play terms
   (`docs/decisions/licensing.md`). This repository is **desktop-only**; the
   mobile distribution path and the mobile *engine* work (native
   AVFoundation/Media3 adapters) are **out-of-repo** — moved to the separate
   mobile repository (future), not planned here (2026-10-03).

## Position

- A Linux distribution should target **Flatpak**: it sidesteps the
  per-distro codec-repo split by bundling the GStreamer runtime, and it keeps
  the GPL source offer tractable. This is a leading candidate, not a decision.
- The small helper libraries (nlohmann/json, toml++, CLI11) are build-time
  `FetchContent` dependencies that compile into the binary; they never ship as
  separate components and need no packaging decision of their own
  (`docs/dependencies.md` §"Small libraries are fetched, not installed").
- Runtime dependencies — Qt6, GStreamer/GES, codec libraries — are declared in
  package metadata and resolved by the package manager; users install one
  package, they do not build (`docs/dependencies.md` §"Build-time vs runtime
  dependencies").

## Undecided

- **Packaging format.** Flatpak is the leading candidate for desktop Linux; no
  format is chosen for any platform (`docs/dependencies.md` §"Not selected").
- **The Extension marketplace re-scope.** GPL copyleft closed the paid model (a
  plugin/linking exception may be added later); how GPL-compatible Extensions are
  distributed and signed is unresolved (`docs/decisions/licensing.md`).
- ~~**Mobile store path.** Whether mobile is ever distributed through the gated
  stores, or only via sideload/F-Droid-class channels, is undecided.~~
  **Moved out-of-repo (2026-10-03)** — this repository is desktop-only; the
  mobile store path belongs to the separate mobile repository (future).
  Qualified counsel review of the license and store gates remains a
  prerequisite to any distribution (`docs/decisions/licensing.md`).
- **Export codec/profile clearance.** The encoder/profile set for export is not
  cleared; it is a test/legal gate, not an approved capability
  (`docs/dependencies.md` §"Engine and codec test gate").

No `packaging/` build recipes or CI matrix exist yet; Phase 9 of
`docs/rewrite-plan.md` is where packaging, the CI matrix, signed releases, and
license review are planned. This record is the constraint statement that work
must satisfy, not the work itself.
