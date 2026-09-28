# Genesis-0 — Engine Adapter Contract

Status: Accepted (2026-09-29).

Date: 2026-09-29. Deciders: project owner + orchestrator.

Scope: the seam between the engine-free host graph and the GES engine adapter — what the host hands
the engine, what comes back, who owns what, and how the boundary is enforced. It is **not** the
engine choice (fixed by T1, `docs/decisions/engine-choice.md`) or the compositor verdict
(`docs/decisions/compositor-path.md`).

---

## 1. Context

Three records force a contract rather than a convention:

- **R5** (`docs/architecture.md` §2.1): the host owns the project/domain/effect graph and rational
  time; adapters compile that graph into an engine pipeline and the host does **not** re-implement
  codecs.
- **R6** (`docs/architecture.md` §2.1): plugin/project APIs never expose GES objects. This is
  the same boundary seen from the Extension/UI side.
- **`docs/decisions/engine-choice.md` §6**: the host owns a portable C++23 model and never leaks
  engine types; the adapter is what makes the engine **swappable**.

`docs/history/rewrite.md` §2 (line 78) makes the adapter load-bearing: the reference `export` crate's
flatten and resolve become the host→engine compilation, with GES types never leaking. §4 Phase 2 names this
document as the planned artifact.

The boundary exists because engine types are C/GObject objects with their own lifetimes and
ownership. A `GESTimeline*` or `GESAsset*` reaching host code would commit the host model, the UI,
and Extensions to GES permanently, and would make adding any second engine a rewrite instead of a
second adapter (no fallback engine is planned, 2026-10-03 — `docs/decisions/engine-choice.md`
§3). `docs/decisions/compositor-path.md` §3/§5 settled
that the `FramePlan` boundary is **not** required for the render path; the host graph → GES
compilation is, independently.

## 2. The contract

### 2.1 What the host hands the engine

A single value: `genesis::render::BuiltTimeline` (`src/render/Resolve.h:132`). It is the resolved
host graph, engine-free by rule:

- a `core::Timeline` (`core/ClipTimeline.h:455`) of `core::Track`s and `core::Clip`s;
- `core::ClipId`-keyed maps for stills, decode sizes, tracks, geometry, colour ranges, and effect
  chains;
- an optional highlight `ClipId`;
- `std::vector<Treatment>` and `std::vector<TransitionSpan>`.

The flattened input it is built from is `src/render/ExportClip.h:80` (`ExportClip`), whose timing
fields are host `core::Rational`. Both headers state the engine-free rule in place
(`ExportClip.h:24-28`, `Resolve.h:28-34`). No GES type appears in either.

The adapter consumes it through one signature: `build_timeline(const
genesis::render::BuiltTimeline&)` (`src/adapters/engine/ges/session/GesBuilder.h:27`). `BuiltTimeline` is
self-contained — its clips carry the media paths, so no `Project` is consulted
(`GesBuilder.h:25-26`).

### 2.2 What the engine hands back

Nothing GES-typed crosses. The session surface is `genesis::adapters::engine::EngineSession`
(`src/adapters/engine/EngineSession.h:18`): `seek(Rational)`, `position() -> Rational`,
`play()`, `pause()`, and `load(const BuiltTimeline&)`. Every parameter and return value is a host
type (`bool`, `core::Rational`, `BuiltTimeline`); the header includes no engine header
(`EngineSession.h:6-10`). The `GESTimeline*` that `build_timeline` returns is the adapter's own
return value and is **never** handed to host code (`GesBuilder.h:21-23`).

The interface is implemented by `GesSession` (`src/adapters/engine/ges/session/GesSession.{h,cpp}`): a
headless playback session that builds the graph with `build_timeline`, prerolls to PAUSED, and
drives the playhead.

### 2.3 Ownership

- `build_timeline` returns a newly created `GESTimeline*` owned by the **caller**, released with
  `gst_object_unref` (`GesBuilder.h:21-22`).
- The GES layer takes ownership of the clip it extracts from an asset; the builder's own asset
  reference is released either way (`GesBuilder.cpp:111-113`).
- The engine keeps **no reference into host memory it does not own** — it copies what it needs
  (`EngineSession.h:36-38`).

The host therefore holds the `GESTimeline*` only as an opaque pointer it must unref; no engine
object outlives its owner across the seam, and no host object's lifetime depends on an engine type.

### 2.4 The conversion seam

The **only** place a host `Rational` becomes an engine time argument is
`to_ns` in `src/adapters/engine/ges/session/GesBuilder.cpp:28-31`: `as_double() * GST_SECOND`, rounded to
`GstClockTime`. `Rational::as_double` is documented **lossy**, for display and for engine
arguments, and explicitly **one-way** (`src/core/ClipTimeline.h:68-71`). No engine time is
converted back to `Rational`.

Time entering the host graph is already exact: flatten/resolve apply the `double` → frame-grid
`quantise` seam (`Resolve.h:197-199`, `Resolve.cpp:236`), so the adapter never sees float seconds
at edit points. Elsewhere, `as_double` appears in host computations such as
`TransitionSpan::progress` and `Treatment::strength_at` (`Resolve.h:77-82,110-120`); those are host
values, not engine arguments, and do not widen this seam.

## 3. The implementation split

The boundary is enforced by the build targets, not by convention:

- **`genesis_host`** (`CMakeLists.txt:34-43`) is a static library built from `src/core/`,
  `src/project/`, and `src/render/`, linking only `nlohmann_json` and `Boost::boost`. It links no
  Qt and no GStreamer.
- **`genesis_engine_ges`** (`CMakeLists.txt:59-64`) is built from `session/GesBuilder.cpp` and
  `session/GesSession.cpp` and links `PkgConfig::GST`, `ges-1.0`, and `genesis_host`. It is the **only
  library** that sees GStreamer/GES (the `genesis_cli` and `genesis_app` executables link it too).
- Only the adapter `.cpp` files include the full GES headers (`GesBuilder.cpp:9-10`,
  `GesSession.cpp:9-10`); the adapter headers forward-declare their GES types so they stay lean
  (`GesBuilder.h:8-10`, `GesSession.h:8-10`).

Because `genesis_host` links no GES library, host code that references a GES symbol does not link,
and the engine headers are absent from the host target's include path. The host test executables
link `genesis_host` alone; only the adapter test executables (`ges_builder_tests`,
`ges_session_tests`) link the adapter (`CMakeLists.txt:85-97`). An engine type attempting to enter
the host is therefore rejected at
build time, which is what makes R5/R6 mechanical rather than aspirational.

## 4. What the contract deliberately does not cover yet

Each line is a later phase, not an omission:

- **Per-clip audio-kind enrichment.** Audio is expressed by `TrackKind`
  (`ClipTimeline.h:307`); a finer per-clip audio classification is not carried in `BuiltTimeline`
  (later phase).

Items this section once deferred are now wired; recorded here as done rather than open:

- **Transitions and layer treatments.** `BuiltTimeline.transitions` and `.treatments` are compiled
  into `GESTransitionClip`s and treatment `GESEffectClip`s with their shader chains
  (`GesBuilder.cpp:502-651`), resolving transition ids through `render/Transitions.h`.
- **Window sink.** GL-memory frames drain through `GesSession::set_gl_context` / `set_frame_sink`
  into the app's `FrameTexture` sink and the `MonitorItem` scene node
  (`src/adapters/engine/ges/session/GesSession.h:23-33`; `src/app/model/MonitorItem.cpp:101-126`).
- **Media pipeline.** Decode preference, cache, proxy, reverse, and filmstrip are implemented
  (`src/render/{MediaCache,ProxyCache,ReverseCache}.cpp`;
  `src/adapters/engine/ges/media/{Proxy,Reverse,Filmstrip}.cpp`) and wired through the app's
  `ProxyController`/`ReverseController`.
- **Effect catalogue.** The pack catalogue and the effect execution path are wired
  (`src/render/Catalogue.cpp`; `src/adapters/engine/ges/effects/ClipEffects.cpp`).

## 5. Consequences

**Cheap now.** Swapping the engine behind the adapter: the host graph (`BuiltTimeline`) and the
session surface (`EngineSession`) are engine-neutral, so a second adapter would implement the same
two entry points without touching host code. The build split keeps that option open, though no
second engine is planned (no fallback engine, 2026-10-03).

**Expensive now.** Anything that wants engine types in the UI or in Extensions: R6 forbids it and
the build split blocks it. Conversely, host code cannot reach engine objects directly; it must go
through the adapter, which is the intended cost.

**Second-engine migration (not planned).** No fallback engine is planned (2026-10-03) and no
second engine is on the roadmap. Should one ever be attempted, the rule still applies: prove the
adapter on **one** engine first (`docs/history/rewrite.md:359-360`), then implement `EngineSession`
and a `build_timeline`-equivalent against it. Effect translation is not free and is absorbed at this
boundary (`docs/decisions/engine-choice.md` §6).

## 6. Revisit triggers

1. A need to widen `EngineSession` beyond host types — the interface is deliberately narrow
   (`EngineSession.h:14-17`).
2. A second engine actually attempted (not planned — no fallback engine, 2026-10-03), which
   reopens the asset-loading and ownership assumptions documented in §2.3.
3. Phase 3 media work needing engine-side handles the current `BuiltTimeline` cannot express.
4. A transition or treatment shape the current `BuiltTimeline` cannot express — the standard GES
   transitions and the treatment effect-clip path are now wired (§4).

## 7. Evidence index

| Fact | Location |
|---|---|
| Engine-neutral session interface, no engine header included | `src/adapters/engine/EngineSession.h:6-18` |
| Session methods use only host types | `src/adapters/engine/EngineSession.h:24-38` |
| Engine copies host data, keeps no foreign reference | `src/adapters/engine/EngineSession.h:36-38` |
| GES session implements the `EngineSession` interface | `src/adapters/engine/ges/session/GesSession.h:22-31`; `GesSession.cpp:61-127` |
| Adapter entry point; GESTimeline owned by caller | `src/adapters/engine/ges/session/GesBuilder.h:18-27` |
| GES types forward-declared; only the adapter .cpp files include them | `src/adapters/engine/ges/session/GesBuilder.h:8-10`; `GesBuilder.cpp:9-10`; `GesSession.cpp:9-10` |
| Own `Rational` → engine time, lossy and one-way | `src/adapters/engine/ges/session/GesBuilder.cpp:25-31` |
| Asset reference released; layer owns the clip | `src/adapters/engine/ges/session/GesBuilder.cpp:108-113` |
| Transitions/treatments compiled by the adapter | `src/adapters/engine/ges/session/GesBuilder.cpp:502-651` |
| Host graph, engine-free (flattened clip) | `src/render/ExportClip.h:24-28,80` |
| Host graph, engine-free (resolved timeline) | `src/render/Resolve.h:28-34,132-161` |
| `double` → frame-grid quantise seam | `src/render/Resolve.h:197-199`; `Resolve.cpp:236` |
| `as_double` documented lossy and one-way | `src/core/ClipTimeline.h:68-71` |
| `genesis_host` links no Qt/GStreamer | `CMakeLists.txt:34-43` |
| `genesis_engine_ges` is the only library linking GStreamer/GES | `CMakeLists.txt:51-64` |
| Host tests link the host alone; adapter tests link the adapter | `CMakeLists.txt:75-80,85-97` |
| `FramePlan` boundary not required for the render path | `docs/decisions/compositor-path.md` §3/§5 |
| Host-owned portable model; adapter makes engine swappable | `docs/decisions/engine-choice.md` §6 |
| R5/R6 text | `docs/architecture.md` §2.1 |
| flatten/resolve become the adapter | `docs/history/rewrite.md` §2 line 78; §4 Phase 2 |
| Prove one engine before a second | `docs/history/rewrite.md:359-360` |
| GES quirks the adapter works around | `docs/dependencies.md` §"Phase 0 spike result" |

See `docs/architecture.md` §6/§7 for the engine strategy and host model, `docs/decisions/engine-choice.md`
for the engine decision this contract sits under, `docs/decisions/compositor-path.md` for the
render-path verdict, and `docs/history/rewrite.md` §4 Phase 2 for the phase acceptance tests.
