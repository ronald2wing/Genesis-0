# Genesis-0 — Engine Choice Decision

Status: Accepted — GES primary; desktop playback gate unrun. Checked: 2026-10-03.

**Re-scope (2026-10-03).** This repository is **desktop-only**. No fallback engine is planned —
GES is the only planned engine — and mobile (Android/iOS) is **out of scope for this repository**,
planned in a **separate mobile repository (future)**. The fallback-engine and mobile-engine material
retained below is historical/re-scoped context, not work planned here.

This registers the engine decision for Genesis-0: which media/timeline engine is primary and which
components sit under the engine. It is a planning/engineering record, not a benchmark result.

---

## 1. Decision summary

- **Primary engine: GStreamer + GES** (GStreamer Editing Services). Chosen because GES is the only
  surveyed candidate that combines a real timeline model (GESTimeline / Layer / Track / Clip /
  Transition / Project), a real plugin ABI, and LGPL on the desktop.
- ~~**A desktop fallback engine.**~~ **Dropped (2026-10-03)** — the desktop GES path works; no
  fallback engine is planned. Historical context (editing maturity and editor precedent) is
  retained in §3.
- ~~**Mobile later = native adapters**, not the desktop engine.~~ **Out-of-repo (2026-10-03)** —
  mobile (Android/iOS) is planned in a separate mobile repository (future); the native-adapter notes
  are retained historically in §4.
- **The host owns a portable timeline/project model in C++** that never leaks engine types
  (GES) to the UI or to Extensions; the engine sits behind an adapter so it can be swapped.
  See §6.
- **GES is the only planned full engine.** Everything else is a component (FFmpeg, libplacebo,
  wgpu, Skia, OTIO) or disqualified. See §7.

## 2. Settled / Provisional / Unresolved

**Settled**
- GES is the primary engine; no fallback engine is planned (2026-10-03).
- Mobile is **out-of-repo** (separate mobile repository, future); this repository is desktop-only.
- The host owns the portable model; engine types never cross the adapter boundary (R5/R6).

**Provisional**
- Desktop render path: **the presentation question is settled (2026-09-29)** — GES output reaches the
  Qt Quick scene graph as GPU-resident `GLMemory` in Qt's own GL context, zero CPU readback
  (`docs/decisions/compositor-path.md`). What remains provisional is cost/performance under load,
  not feasibility.
- The set of components under the engine (FFmpeg/libplacebo/OTIO choices) is planned, not validated.
- ~~A desktop fallback engine remains available.~~ **Dropped (2026-10-03)** — no fallback engine
  is planned.

**Unresolved (gates — must not be assumed closed)**
- ~~**GES desktop render-into-Qt-Quick spike has not been run.**~~ **Resolved (2026-09-29): the spike
  passed.** GES output presents in a Qt Quick window as GPU-resident `GLMemory` in Qt's own GL
  context, zero CPU readback, running to EOS without GL errors
  (`docs/decisions/compositor-path.md`). What is *not* claimed is a cost/performance figure under
  production load — feasibility is settled, cost is a profiling question.
- ~~**Mobile has not been tested** on real Android/iOS hardware.~~ **Out-of-repo (2026-10-03)** —
  mobile is planned in a separate repository; GStreamer's official mobile *deployment* documentation
  is not evidence of *app performance* or parity either way, but it is not this repo's gate.
- **No performance claims** are made for any engine or platform.
- The desktop device/engine gate (playback/seek/timeline/composite/export) remains unrun; no
  fallback engine is planned, so there is no fallback trigger to decide. Mobile is out-of-repo (see
  `docs/architecture.md` §6 and `docs/history/implementation.md` Phase 1).

## 3. Primary engine (no fallback)

| Candidate | License | Timeline model | Desktop | Verdict |
|---|---|---|---|---|
| **GStreamer + GES (primary)** | LGPL | GESTimeline / Layer / Track / Clip / Transition / Project | Supported (desktop spike passed 2026-09-29) | Selected as primary |

**No fallback engine (2026-10-03).** The desktop GES path works, so no second engine is planned.
Never two engines at once (`docs/architecture.md` §6). Mobile is out-of-repo and is no longer a
factor in engine selection.

## 4. Future mobile path (out-of-repo, historical)

**Re-scoped (2026-10-03):** mobile (Android/iOS) is **out of scope for this repository** and is
planned in a **separate mobile repository (future)**. The native-adapter material below is retained
as historical context, not as work planned here.

- **iOS:** Apple AVFoundation / AVComposition for composition, plus Core Image / Metal for effects.
- **Android:** AndroidX Media3 / Transformer for composition/transcode, plus MediaCodec for codecs.
  Media3 is Apache-2.0.
- **Do not plan on ffmpeg-kit for mobile:** ffmpeg-kit is **retired/archived (2026)**; the successor,
  **FFmpegKitNext**, is **source-only**. Any mobile FFmpeg route is a build-from-source obligation,
  not a drop-in dependency.

Native adapters keep a single host-owned model while letting each platform use its first-class media
stack. They are **provisional/historical**, not implemented or tested here.

## 5. Components under the engine

These are components, not full engines:

- **FFmpeg** — codecs/containers, used as an **LGPL build**; the build must **avoid `--enable-gpl`**
  to keep obligations controllable (`docs/decisions/licensing.md` §3/§6). Note the dev-host FFmpeg is
  a GPL build (`docs/dependencies.md`) and is not a cleared distribution basis.
- **libplacebo** — HDR / GPU processing, LGPL.
- **OpenTimelineIO (OTIO)** — **optional interchange only** (Apache-2.0), never the guaranteed
  lossless host project format (`docs/architecture.md` §4).
- **wgpu / Skia** — only if a custom compositor is ever needed; not selected.

## 6. Architecture constraint (host-owned portable model)

- The host defines the project/domain/effect graph and rational time in standard C++23 and **never
  exposes GES objects to the UI or to Extensions** (R5/R6).
- The engine sits behind an **engine adapter** that compiles the host graph into an engine pipeline
  and does not re-implement codecs (R5).
- The adapter boundary is what makes the engine **swappable**; engine swapping and effect
  translation are **not free**, and that cost is absorbed at this boundary.

## 7. Survey result

- **No open-source engine beats GES as a full engine.** The rest of the field splits into:
  - **Components:** FFmpeg, libplacebo, wgpu, Skia, OpenTimelineIO.
  - **Disqualified:** libmpv (GPL core); several GPL editors; libVLC (playback-only);
    a smaller experimental engine library.
- **Clone survey:** Qt is the mainstream UI across the surveyed editors, and the dominant engine
  is another framework; **GES is used by only one surveyed editor**, which documented why it is
  hard — roughly ~20k lines deleted, performance concerns, and an upstream-first posture. That
  precedent is a caution for GES, not evidence against it.

## 8. Reference-only sources (design references, not code sources)

- **The reference editor** is now a **code source** for Genesis-0, per the licensing override
  recorded in `docs/decisions/licensing.md`. Genesis-0 is licensed GPL-3.0-or-later, so reference
  code may be copied or adapted; copied files remain GPL-3.0 and must retain their notices. Note the
  stack mismatch: the reference editor is a Rust engine with a GPU compositor, while Genesis-0 is
  C++23 + Qt6
  Quick/QML + GStreamer/GES, so incorporation is a port, not a drop-in.
- **GPL clones** remain **design references only, not code sources**. GPL-3.0 code may be copied into
  a GPL-3.0-or-later work, but GPL-2.0-only code is **not** compatible; no GPL clone code is copied
  without a per-file license check.
- Per `docs/decisions/licensing.md`, GPL/LGPL study does not grant permission to copy unless the
  license permits it; GPL-3.0 permits copying into a GPL-3.0 work.

## 9. Consequence for the other docs

- `docs/architecture.md` §2.2 (T1/T2), §4 diagram, §6, and §17 matrix must read **GES primary /
  no fallback engine (2026-10-03)**, with mobile out-of-repo — not "GES first candidate / fallback
  unclear".
- `docs/history/implementation.md` Phase 1 runs the **GES desktop spike**; no fallback-engine probes
  are planned and no mobile gate lives here. The desktop device gate remains unrun.

## 10. Source appendix (external references)

Checked 2026-09-28. All entries are **external references**; none is a local file. Each ID is
referenced by the sections above.

| ID | Reference | URL (external) |
|---|---|---|
| E1 | GStreamer GES documentation (timeline model) | `https://gstreamer.freedesktop.org/documentation/gst-editing-services/index.html` |
| E2 | GStreamer — installing for Android development | `https://gstreamer.freedesktop.org/documentation/installing/for-android-development.html` |
| E3 | GStreamer — installing for iOS development | `https://gstreamer.freedesktop.org/documentation/installing/for-ios-development.html` |
| E6 | FFmpeg — legal / license information | `https://www.ffmpeg.org/legal.html` |
| E7 | ffmpeg-kit (retired/archived 2026) | `https://github.com/arthenica/ffmpeg-kit` |
| E8 | OpenTimelineIO (optional interchange, Apache-2.0) | `https://github.com/AcademySoftwareFoundation/OpenTimelineIO` |
| E9 | libplacebo (HDR/GPU, LGPL) | `https://github.com/haasn/libplacebo` |
| E10 | AndroidX Media3 / Transformer (Apache-2.0) | `https://developer.android.com/media/media3` |
| E11 | Apple AVFoundation | `https://developer.apple.com/av-foundation/` |

See `docs/architecture.md` §6/§17 for the engine strategy and capability matrix, and
`docs/history/implementation.md` Phase 1 for the spike and gate.
