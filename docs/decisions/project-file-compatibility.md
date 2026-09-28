# Genesis-0 — Project-File Compatibility with Concat (Decision Record)

Note: host file paths below are cited against the post-2026-10-refactor layout; Concat citations
(`doc.rs`, `model.rs`) are unchanged.

Status: **Proposed** (2026-09-29). Decision material for the project owner, not a decision. Checked:
2026-09-29. No Concat document was executed against a Genesis-0 build; all Concat claims below are
read from source at the cited lines. No performance, parity, or byte-compatibility claim is made.

Deciders: project owner (to decide) + orchestrator.

Companion evidence: `docs/rewrite-plan.md` §3(d) and §7 Q3; `docs/rewrite-plan.md` Phase 1 status;
`docs/architecture.md` §1, §4, §7; `docs/decisions/host-core-language.md` (§5 locking-in list);
`docs/decisions/licensing.md` §0/§5.

Scope: whether Genesis-0 reads and/or writes Concat's `concat.json` project documents, and how
edit-point times cross the `f64`-seconds/`Rational` boundary. It does **not** decide the host-native
schema, its file extension, or the `extra`/forward-compat design; those follow from the choice here.

---

## 1. Context

### 1.1 What the Concat document is

The document is JSON, named `concat.json`, read and written by `concat-project`'s `doc.rs`
(`concat/src/crates/concat-project/src/doc.rs:4`). Two identity fields sit at the top: `concat`
(the writer emits `"0.1.0"`, `doc.rs:317`) and `version` (`DOCUMENT_VERSION = 1`, `doc.rs:41`). The
wire spelling is `camelCase` — "Serde names are camelCase: that is the document's spelling"
(`concat/src/crates/concat-project/src/model.rs:12`); e.g. `trackId`, `sourceStart`, `fadeIn`,
`videoEffects`, `rateNum`.

Edit-point times in the document are **`f64` seconds**, not rationals. Concat states this as a
deliberate freeze: "Times are `f64` seconds here because that is what the on-disk format stores,
and the documents that exist freeze the format. The conversion to exact rationals stays at the
render boundary... Moving the *document* to rational time is a format decision for a deliberate
version 2" (`model.rs:6-10`). The crate's own preamble repeats it: "`f64` seconds and String ids
are the document's terms, kept until a version 2 decides otherwise on purpose"
(`concat/src/crates/concat-project/src/lib.rs:17-19`).

Reading is three steps with one job each (`doc.rs:8-24`): **migrate**, **parse** (the model's own
serde derives; missing fields default, each list keeps the entries that parse and drops the rest,
each unknown field lands in the struct's `extra` map and is written back untouched), then **settle**
(the decisions no derive can make: drop a clip whose track or media is gone, drop a timeline with no
lane, clamp numbers, ensure the active timeline exists).

`document_version` reads the `version` field and defaults an absent one to **1** — "that is the
version their shape was given" (`doc.rs:45-47`).

### 1.2 The migration paths that exist

A single ordered table, applied oldest shape first (`doc.rs:62-71`). Two migrations exist:

1. **`lift_flat_timeline`** (`doc.rs:77-102`): a document from before multiple timelines holds one
   as top-level `tracks` / `clips` / `video`. If `timelines` is absent or empty and top-level
   `tracks` is non-empty, the flat fields are lifted into `timelines` as the one timeline
   (`id: "TL1"`, `name: "Timeline 1"`), leaving the flat fields in place as the mirror every save
   writes (`doc.rs:90-101`). Covered by `a_legacy_flat_document_loads_as_one_timeline`
   (`lib.rs:2223`).
2. **`give_timelines_their_frame`** (`doc.rs:108-136`): timelines from before each carried its own
   frame take the document's top-level `video` block, field by field; a zero/absent dimension or
   rate takes the shared value (`doc.rs:123-131`). Covered by
   `a_timeline_without_its_own_frame_takes_the_documents` (`lib.rs:1458`).

A document from a **later** build is refused whole, not read in part and saved over
(`doc.rs:140-143`); covered by `a_document_from_a_newer_build_is_refused` (`lib.rs:1183`). New
format changes are meant to go through `DOCUMENT_VERSION` and append a migration; the table is
never reordered or removed (`doc.rs:59-61`).

### 1.3 Unknown and unparseable entries: dropped or tolerated, never fatal

The standing rule is that "a hand-edited or older file degrades to something openable. Nothing short
of a document that is not an object, or that has no timeline in it, fails the load"
(`doc.rs:22-24`). Concretely:

- An unknown field is kept in the owning struct's `extra` map and written back unchanged
  (`doc.rs:12-16`; `serde(flatten)` at `model.rs:187-188`). Covered by
  `fields_this_build_does_not_know_survive_a_round_trip` (`lib.rs:1418`).
- Entries that do not parse are **dropped individually**, not escalated to a document failure:
  media without an id, a track without an id, clips whose `start` is not a number, out-of-range
  keys (`an_entry_the_reader_cannot_parse_is_dropped_not_the_document`, `lib.rs:1488`).
- A clip with no id, a vanished track, or (for file-backed kinds) missing media is dropped on load;
  text and layer clips need no media and survive
  (`clips_for_vanished_tracks_or_media_are_dropped_on_load`, `lib.rs:3568`; logic at
  `doc.rs:252-276`).
- Hand-edited numbers are pulled into the ranges a command would have held them to, via `Clip::tidy`
  (`hand_edited_values_are_clamped_on_load`, `lib.rs:3599`; `doc.rs:275`).
- A document that is not an object, or that has no surviving timeline, returns `None` rather than
  panicking (`a_garbage_document_is_none_not_a_panic`, `lib.rs:2641`; `settle` returns `None` when
  no timeline survives, `doc.rs:205-207`).

Semantic round-trip is covered by `the_document_round_trips` (`lib.rs:2127`, asserting
`restored.project() == editor.project()`) and a v1-shape fixture by `a_version_one_document_loads`
(`lib.rs:2150`). **Caveat:** these assert model equality and unknown-field survival, not byte
identity; no test in the read-only checkout establishes that a save reproduces the input bytes.

### 1.4 The writer

`to_document` (`doc.rs:314-361`) emits the writer-owned keys (`doc.rs:154-165`): `concat`,
`version`, `name`, a top-level `video` block holding the **active timeline's** frame
(`doc.rs:323-331`), `media`, then a **flat mirror** of the active timeline's `tracks` and `clips`
(`doc.rs:336-345`), `fonts`, every `timelines` entry, and `activeTimelineId`. The flat
`tracks`/`clips`/`video` mirror exists so a build from before multiple timelines still reads the
file (`doc.rs:26-28`). Foreign top-level fields the reader did not own are written back only over
keys the writer does not own (`doc.rs:355-359`).

### 1.5 The host model on the other side

The ported host model already carries **`Rational` time** for the edit points. `Clip::start` /
`duration` / `source_start` and `Clip::fade_in` / `fade_out` are `Rational`
(`src/project/model/Clip.h`), as are `Transition::duration` (`src/project/model/Speed.h`),
`Stroke::at` (`src/project/model/Cutout.h`), and `MediaItem::duration`
(`src/project/model/Media.h`) — the eight fields the rewrite plan records as migrated
(`docs/rewrite-plan.md:313-316`). `Rational` is an exact `int64 num/den` in lowest terms; its
serial spelling is `"num/den"` (or a bare integer when the denominator is 1)
(`src/core/ClipTimeline.cpp:228-231`), and its parser accepts `"num/den"`, a bare integer, or a
decimal (`src/core/ClipTimeline.h:41-44`). It provides an explicit `approximate(double)` seam that
rounds to the nearest rational with denominator 1,000,000 (`ClipTimeline.cpp:187-195`) and warns
that `as_double()` is lossy and "a value that round-trips through double is no longer exact"
(`ClipTimeline.h:68-71`).

Not every number is a `Rational`: key positions (`ClipKey::at`, `src/project/model/Keyframe.h`) and
transform amounts remain `double`, and the `extra` maps that `doc.rs` round-trips are **not ported** —
the model carries a placeholder comment saying so (`src/project/Project.h`). The serialization layer
(`doc.rs`) is unported
(`docs/rewrite-plan.md:315-316`), and the ~20 document tests are gated on this question
(`docs/rewrite-plan.md:324-326`). The host project file is the source of truth; OTIO is optional
interchange only, "not a guaranteed lossless host project format" (`docs/architecture.md:109-111`).
Genesis-0's own portability requirement — "Portable project files open on any of the five
platforms" (`docs/architecture.md:255`) — is about Genesis-0, not Concat compatibility.

## 2. The question

Stated exactly as `docs/rewrite-plan.md` §7 Q3 (`docs/rewrite-plan.md:541-542`):

> **Project-file compatibility.** Must Genesis-0 read/migrate existing `concat.json` documents
> (one-way migration), or is this a new format with no Concat file compatibility? (§3(d).)

The companion constraint is §3(d) (`docs/rewrite-plan.md:185-200`): Concat freezes `f64` seconds in
the document and converts to exact `Rational` only at the render boundary, quantised to the frame
grid; Genesis-0 stores rational time everywhere with "no float seconds for edit points"
(`docs/architecture.md:147`). Any compatibility path must cross that boundary somewhere.

## 3. Options

### (a) Full compatibility — read and write Concat documents, round-trip compatible

**What it requires.** Port `doc.rs` whole: the tolerant reader **and** `to_document`, including the
flat `tracks`/`clips`/`video` mirror (`doc.rs:336-345`), the `WRITER_OWNED` scan
(`doc.rs:154-165`), and the per-struct `extra` maps that keep foreign fields alive
(`doc.rs:355-359`). To keep Concat able to open the result, `version` must stay `1` and must not be
bumped, or Concat refuses the file whole (`doc.rs:140-143`).

**Cost / benefit.** Benefit: a Genesis-0 project opens in Concat, and the two editors can coexist
over one file. Cost: the host must emit `f64` seconds on disk, which forces `Rational::as_double`
at every save — the conversion the host code documents as lossy and non-invertible
(`ClipTimeline.h:68-71`). Rationals off the decimal grid (e.g. 1/30 s, 1/60 s frame durations) are
not exactly representable, so either round-trips are inexact or the host keeps a shadow `f64`
representation. This directly conflicts with `docs/architecture.md:147` and with Phase 1 acceptance
1 ("Rational edit points round-trip through serialization; no float seconds at edit points",
`docs/rewrite-plan.md:300-302`). It also requires porting the `extra` maps, which nothing else in
the host needs, and pins the on-disk schema to Concat's v1 forever.

**Forecloses.** A host-native format with exact rational edit points; a `version`-bump evolution
under Concat's field; freedom to drop the flat-mirror/`extra` machinery.

### (b) Import / migrate only — read a Concat document into a host-native format; never write Concat format again

**What it requires.** The tolerant reader plus the two migrations (`doc.rs:62-136`), `settle`
(`doc.rs:178-276`), and one conversion of each `f64`-seconds edit point into `Rational` at load —
the `approximate` seam already exists for exactly this (`ClipTimeline.cpp:187-195`). The host then
writes its **own** format with exact `num/den` times. `extra` preservation becomes optional (it
matters only if a lossless re-export is ever wanted).

**Cost / benefit.** Benefit: existing Concat projects are not stranded, while the host format stays
exact and is free to evolve under its own versioning. The cost is bounded to a one-way reader; the
writer half of `doc.rs` and the `extra` round-trip machinery are not a standing obligation. This
satisfies Phase 1 acceptance 1 (the host-native writer is needed anyway) and unblocks the ~20
gated document tests, adapted to import semantics.

**Forecloses.** Concat reading Genesis-0 output. It also spends most of the tolerant-reader cost
(migrations, `settle`, per-entry drops) for a potentially empty installed base.

### (c) No compatibility — a host-native format from day one; Concat documents are unreadable

**What it requires.** Only a host-native reader/writer and its round-trip tests. No migrations,
no Concat v1 shapes, no `settle` of foreign documents, no `extra` maps.

**Cost / benefit.** Cheapest, and cleanest: the format is designed once, for `Rational` time, and
never carries Concat's tolerance rules. It satisfies Phase 1 acceptance 1 with the least code. Cost:
any existing `concat.json` user's work cannot be opened.

**Forecloses.** Any migration path for existing Concat documents; the option of capturing Concat
users by opening their files. Re-adding a reader later is possible but is the same work as (b),
done after a format has already shipped.

### 3.1 Rational-time serialization across the boundary

The host stores edit points as exact `Rational`; Concat stores them as `f64` seconds. Where the
boundary is crossed differs per option, and it is the decisive technical detail:

- **Under (a)**, the boundary is crossed on **every save**: `Rational → f64` (`as_double`,
  `ClipTimeline.cpp:224-226`). The host's own documentation forbids converting back
  (`ClipTimeline.h:68-71`), so a load-then-save cycle cannot be exact for non-dyadic rationals.
  Decimal seconds on disk is precisely the failure mode `docs/architecture.md:147` exists to avoid.
- **Under (b)**, the boundary is crossed **once, at import**: `f64 → Rational`. The honest choices
  are (i) exact `num/den` spelling via `approximate`, which snaps to the nearest 1/1,000,000 — this
  is the documented seam and lands within well under a frame, but it is not bit-exact to the
  original `f64` (a frame duration of 1/30 s imports as 33333/1000000, ~3.3e-10 s off), or (ii)
  carry the original `f64` alongside to preserve re-export fidelity. The host-native writer then
  emits exact `num/den` (`ClipTimeline.cpp:228-231`), and no further `double` round-trip occurs.
- **Under (c)**, the boundary is never crossed.

Note the boundary is narrow: only the **eight edit-point fields** are rational. Key positions
(`ClipKey::at`, `src/project/model/Keyframe.h`) and transform amounts are fractions and stay
`double` in the host, matching Concat's key model, which is anchored through edits as fractions of
clip length rather than as seconds (`docs/rewrite-plan.md:188-190`). "No float seconds for edit
points" does not extend to those.

A second, separately-tested boundary is the **command wire**. `commands_arrive_in_camel_case`
(`lib.rs:2263`) shows the same `camelCase` JSON is the command surface, not only the document
(`docs/rewrite-plan.md` §2 maps `concat-api` onto the host command layer). If the host adopts
Concat's command spelling too, that is a further compatibility surface this record does not decide.

## 4. Recommendation

**Recommend (b): import/migrate only.** Read `concat.json` through a ported tolerant reader and the
two migrations, convert edit points once via `Rational::approximate`, and write a host-native format
with exact `num/den` times. Do not write Concat's format.

**Strongest argument for.** It is the least that neither strands existing Concat work nor violates
`docs/architecture.md:147`. The reader is the larger half of `doc.rs` and is needed by (a) and (b)
alike; the writer and the `extra` round-trip machinery are the only parts (b) drops, so import
captures most of full compatibility's user-facing benefit at most of its reader cost — while keeping
exact rational time on disk and satisfying Phase 1 acceptance 1 (`docs/rewrite-plan.md:300-302`).

**Strongest argument against.** It still pays the full tolerant-reader cost — migrations, `settle`,
per-entry dropping, and the adapted tests — for an installed base that this record cannot confirm
exists; and if the product ever wants Concat to open Genesis-0 files, (b) must be superseded by (a)
later, costing a second format transition. If neither existing `concat.json` files nor
bi-directional interchange are product requirements, **(c) is strictly cheaper** and is the correct
answer; the owner should not adopt (b) merely by default. **(a) is not recommended**: writing `f64`
seconds to disk contradicts the host's stated rational-time contract and makes exact round-trip
unachievable.

## 5. Consequences

**What this locks in (if (b) is chosen).**
- A host-native on-disk format with exact `num/den` edit points, consistent with
  `docs/architecture.md:147` and the Phase 1 model work (`docs/rewrite-plan.md:313-316`).
- An import path that ports the tolerant reader, both migrations, and `settle`; the ~20 gated
  document tests (`docs/rewrite-plan.md:324-326`) are adapted to import semantics rather than
  Concat's write-back semantics.
- One-way only: Concat cannot read Genesis-0 projects.

**What this forecloses.**
- Bi-directional interchange unless (a) is later adopted.
- Concat's `extra`/flat-mirror write-back fidelity is not preserved as a standing obligation.

**What would trigger a revisit.**
1. **An installed base is confirmed to matter** *and* bi-directional exchange with Concat becomes a
   product goal — this reopens the choice between (a) and (b).
2. **No installed base is confirmed** — then (c) supersedes (b) and this record should be reversed
   before `doc.rs` is ported.
3. **The frame-accurate fidelity of imported times is shown insufficient** at the
   `approximate` 1/1,000,000 seam — this forces option (a)'s shadow-`f64` or a different import
   representation.
4. **The host-native format is designed** such that its schema diverges from Concat's beyond
   time representation (IDs, track model, effect references) — import remains possible, but the
   migration's tolerance rules must be re-derived against Concat's `settle` and the ported tests.

## 6. Open questions for the project owner

Each of these is a decision, not a research item.

1. **Installed base.** Do existing `concat.json` documents need to open at all? If no, choose (c)
   and skip the reader.
2. **Direction.** Must Concat be able to open Genesis-0 projects (bi-directional), or is one-way
   import sufficient? Bi-directional selects (a) with its `f64`-on-disk cost.
3. **Time on disk.** Confirm that `docs/architecture.md:147` ("no float seconds for edit points")
   governs the host-native **file format** as well as the in-memory model — and that key positions
   and transform amounts (already `double`) stay `double`.
4. **Import fidelity.** For imported edit points, is snapping to the nearest 1/1,000,000
   (`Rational::approximate`) acceptable, or must the original `f64` be retained for exact
   re-export?
5. **Unknown fields.** Must forward-compatible fields that Genesis-0 does not model survive an
   import (requiring the `extra` maps), or may they be dropped at import?
6. **Trigger.** Is migration automatic on open, or an explicit import action? This affects whether
   the host ever needs to distinguish "a Concat document" from "a Genesis-0 document" on disk.

---

### Evidence index

| Fact | Location |
|---|---|
| Document is `concat.json`, JSON | `concat/.../doc.rs:4` |
| `version` = `DOCUMENT_VERSION` = 1 | `doc.rs:41`, `doc.rs:318` |
| Absent `version` reads as 1 | `doc.rs:45-47` |
| camelCase wire spelling | `concat/.../model.rs:12` |
| `f64` seconds frozen in the document | `model.rs:6-10`; `lib.rs:17-19` |
| Reader: migrate / parse / settle | `doc.rs:8-24` |
| Migration table (2 entries) | `doc.rs:62-71` |
| `lift_flat_timeline` | `doc.rs:77-102` |
| `give_timelines_their_frame` | `doc.rs:108-136` |
| Later version refused whole | `doc.rs:140-143` |
| Tolerance: unknown fields survive | `doc.rs:12-16`; test `lib.rs:1418` |
| Tolerance: bad entries dropped | tests `lib.rs:1488`, `lib.rs:3568` |
| Tolerance: values clamped | test `lib.rs:3599`; `doc.rs:275` |
| Non-object / no timeline → `None` | `doc.rs:22-24`, `doc.rs:205-207`; test `lib.rs:2641` |
| Semantic round-trip / v1 fixture | tests `lib.rs:2127`, `lib.rs:2150` |
| Writer keys and flat mirror | `doc.rs:314-361`, `doc.rs:154-165` |
| Host edit points are `Rational` (8 fields) | `src/project/model/{Media,Cutout,Speed,Clip}.h`; `docs/rewrite-plan.md:313-316` |
| `Rational` spelling `num/den`; parser | `src/core/ClipTimeline.cpp:228-231`; `ClipTimeline.h:41-44` |
| `approximate(double)` → /1,000,000 | `src/core/ClipTimeline.cpp:187-195` |
| `as_double()` documented lossy | `src/core/ClipTimeline.h:68-71` |
| Key positions/amounts remain `double` | `src/project/model/Keyframe.h` |
| No `extra` maps; document layer drops unknown fields | `src/project/Project.h`; `src/project/document/` |
| `doc.rs` unported; ~20 tests gated | `docs/rewrite-plan.md:315-316,324-326` |
| Phase 1 acceptance 1 | `docs/rewrite-plan.md:300-302` |
| Question §7 Q3 / constraint §3(d) | `docs/rewrite-plan.md:541-542`, `185-200` |
| Rational-time requirement; host file is source of truth | `docs/architecture.md:147`, `109-111` |
| Command wire is camelCase | test `concat/.../lib.rs:2263` |
