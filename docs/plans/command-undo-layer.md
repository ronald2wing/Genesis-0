# Genesis-0 — Command + Undo Layer (Planning Document)

Status: historical planning record — implemented as of 2026-10-01; file/line citations predate the model/command/document split.
Note: file names below predate the 2026-10 refactor.

Companions: `docs/architecture.md` (§7 host model; R5/R6/R11), `docs/rewrite-plan.md` (§2
`concat-project`/`concat-api` rows, §3(c) undo, §3(d) times, §7 Q2/Q3), `docs/implementation-plan.md`
(Phase 2 acceptance). Terminology follows `docs/architecture.md` §1.

Source of truth for the port is the read-only checkout
`/home/bigbrother/Projects/video-editors/concat` (referred to below as `concat-project/src/...`).
Ground truth in this repository: `src/project/ModelTypes.h` (the ported project model, compiles
clean), `src/core/ClipTimeline.h` (`Rational`, `FrameRate`, `TimeRange`), `src/core/Animation.h`,
`src/core/SpeedCurve.h`.

---

## 0. How to read this plan

- §1 fixes the command model (C++ shapes, application, composition, ownership).
- §2 fixes undo/redo as a command stack and how a command's inverse is represented.
- §3 fixes rational time at edit points and the migration cost.
- §4 is the per-verb inventory against Concat's `Command` enum.
- §5 marks the serialization seam without designing it.
- §6 is the file layout; §7 verification; §8 the questions the user must answer.

Verdicts in §4: **port-as-is** = the verb's control flow and arithmetic are unchanged and only the
time representation or a shared helper moves; **re-derive** = the algorithm's structure changes
(rewindow over rationals, split source map, ripple exactness, speed mean, engine coupling);
**drop** = not carried as a recorded command.

---

## 1. Command model

### 1.1 Shape: `std::variant`, not a class hierarchy

Concat's `Command` is a serde-tagged enum — one value type with a tag and per-variant fields
(`concat-project/src/commands/mod.rs:224-231`). The C++ mirror is a closed `std::variant` of plain
per-verb structs, all in namespace `genesis::project`:

```
using Command = std::variant<
    AddMedia, RemoveMedia, SetMediaPlaceholder, FillSlot, Batch,
    AddClip, AddClipAtFirstFree, AddTextClip, AddLayerClip,
    SetClipSpeedCurve, SetClipKey, ClearClipKey, ClearClipKeys,
    SetEffectKey, ClearEffectKey, ClearEffectKeys,
    SetClipCutout, AddCutoutStroke,
    MoveClips, TrimClip, SplitClips, ReplaceClipMedia, FreezeFrame,
    MergeClips, RemoveClips, UpdateClip, SetClipSpeed, SetClipTransform,
    DetachAudio, ReattachAudio,
    AddTrack, RemoveTrack, SetTrackFlag,
    AddTimeline, RemoveTimeline, SetTimelineVideo, RenameTimeline,
    SelectTimeline, MoveTimeline,
    AddFont, RemoveFont, UpdateMediaPath, SetMediaColorRange>;
```

Rationale, in order of weight:

1. **Value semantics for history.** A `HistoryEntry` stores a `Command` by value; a variant of
   structs is trivially copyable when its members are. A virtual base forces heap/`unique_ptr`
   ownership and indirection into every history record for no benefit.
2. **Exhaustive routing.** `std::visit` reproduces Concat's `match` (mod.rs:989-1054) and fails to
   compile when a variant is added without a handler — the same property the Rust match gives.
3. **Serde parity.** The tagged-enum shape maps onto the `{"op": ..., ...}` wire form the API layer
   needs (`docs/rewrite-plan.md` §2 `concat-api` row), without committing to a JSON library now.
4. **No open set.** Commands are not a third-party extension point; a closed variant is correct.

Supporting value structs mirror Concat: `NewMedia` (mod.rs:179-218), `ClipPatch` with the three-way
`absent | null | value` semantics (mod.rs:85-153), `ClipMove` (mod.rs:69-78), `TrimEdge`
(mod.rs:48-54), `TrackFlag` (mod.rs:57-64), `Outcome` (mod.rs:694-706), `CommandError`
(mod.rs:713-746).

### 1.2 Application: `std::expected`

```
std::expected<Outcome, CommandError>
apply(const Command&, Project&, IdMinter&, UndoRecord&);
```

- `std::expected` is C++23 (R11) and matches Concat's `Result<Outcome, CommandError>` directly.
- `Outcome { std::optional<std::string> created_id; bool applied; }`. `applied == false` is the
  tolerated no-op that must record no history (mod.rs:694-706, editor.rs:143-145).
- `CommandError` is a small enum carrying the user-facing sentence Concat treats as contract
  (mod.rs:708-746). Decision Q8: whether those sentences stay in the model layer or move to the UI
  layer; default is to keep them next to the error that produces them, as Concat does.
- `IdMinter` (mod.rs:752-795) is owned by the `Editor` and passed by reference; `adopt_project`
  (mod.rs:781-794) advances it past every id a restored file uses.

The `apply` free function is the dispatcher; each verb group lives in its own translation unit
(§6). Non-finite guards (`Command::has_non_finite`, mod.rs:843-961) become a free
`has_non_finite(const Command&)` checked once at the top of `apply`, before dispatch — JSON cannot
spell NaN but a caller can, and one stored poisons every duration it touches.

### 1.3 Composition: `Batch`, atomic without a whole-project clone

`Batch` is a `Command` variant holding `std::vector<Command>`; nesting is legal (mod.rs:266-269).
Concat applies a batch to a staged **clone** of the whole project and commits only on full success
(mod.rs:990-1009). The host does not clone the project: it applies members in order while
accumulating the pending `UndoRecord`, and on a member failure reverts the accumulated record and
returns the error. The observable contract is identical (all-or-nothing, one undo step); the cost
is the touched delta instead of the project size.

### 1.4 Ownership and boundaries

- `Editor` owns `Project`, `IdMinter`, and `History` **by value** (mirrors Concat's `Editor`,
  editor.rs:45-52). No `shared_ptr`, no `Arc`. Concat needed `Arc<Clip>`/`Arc<Timeline>`
  (`concat-project/src/model.rs:1662,1821`) only to make snapshot undo cheap; §2 removes that
  requirement, so the host model stays plain values, as it already is (`src/project/ModelTypes.h`).
- The UI and the future API layer never see a mutable `Project`; all mutation goes through
  `Editor::apply`, so nothing changes without a recorded inverse (editor.rs:109-113).
- No engine type crosses this layer (R6). Commands are pure functions over the project model.

---

## 2. Undo / redo: a command stack with scoped before-images

Concat's undo is whole-state snapshots with depth 200 (`editor.rs:32-52`), cheap because of `Arc`
sharing. Architecture §7 requires commands over the domain model, and rewrite-plan §3(c) sets the
acceptance bar: **undo/redo restores graph state exactly**, including the coupled HDR step, with
gesture coalescing and view-state exclusion. Without `Arc`, whole-state snapshots per edit are
O(project) each and defeat the intent, so the host records a **scoped delta**.

### 2.1 Stack structure

```
struct HistoryEntry {
    Command command;              // forward edit, replayed on redo
    UndoRecord before;            // the inverse: before-images of what it wrote
    std::optional<std::string> gesture;
};

class History {
    std::deque<HistoryEntry> undo_;   // oldest at the front
    std::deque<HistoryEntry> redo_;
};
```

- Depth 200 is a **product decision to restate**, not inherit silently (rewrite-plan §3(c),
  §7 Q2). The default in the plan is 200 for behavioral continuity.
- Eviction is from the front of `undo_` (editor.rs:157-159).

### 2.2 How the inverse is represented: before-images, not mirrored commands

`UndoRecord` is an ordered list of entity before-images plus the two counters that are not in the
model:

- `id_counter` — the `IdMinter` counter before the edit.
- `active_timeline_id` — before the edit.
- `std::vector<ClipSnapshot>` / `TrackSnapshot` / `TimelineSnapshot` / `MediaSnapshot` /
  `FontSnapshot`, each `{ std::string id; std::optional<T> before; }`. `nullopt` means *absent
  before* (undo removes it); a value means *present before* (undo replaces or re-inserts it).

Undo walks the record in reverse and restores each entity; redo re-applies the stored `Command`.
Because `apply` is deterministic over `(Project, IdMinter)` and the counter is restored, re-running
the forward command reproduces the after-state exactly — so no after-image is stored.

**Why before-images and not a mirrored inverse command.** Three operations are derived or lossy and
cannot be inverted by re-running a symmetric algorithm:

- `Clip::rewindow_keys` re-anchors every key through split/trim/merge (`ModelTypes.h:1367`,
  `absorb_keys` at `1452`); the inverse is not the same call with swapped arguments.
- `tidy_touched` runs `Clip::tidy` over every clip the command replaced (`editor.rs:170-197`); the
  clamps (`ModelTypes.h:1096-1204`) are not a bijection.
- `follow_first_hdr` promotes a timeline to HLG inside the step that added its first HDR clip
  (`editor.rs:205-247`); its inverse is not a command, it is "the colour it had".

Storing the before-image guarantees exact restore by construction. This is the per-edit
snapshot-equivalence behind the same interface that rewrite-plan §3(c) explicitly permits; the
plan chooses it for the derived cases rather than discovering divergence later.

**Cost.** Only entities the command wrote are copied. A multi-clip move copies the moved clips; a
timeline flag flip copies one timeline header. This matches Concat's "copies only the clip it
writes" property (editor.rs:5-10) without `Arc`.

### 2.3 Application and history in one call

```
Editor::apply(command)                 -> apply_within(std::nullopt, command)
Editor::apply_within(gesture, command) -> std::expected<Outcome, CommandError>
```

Sequence (mirrors editor.rs:133-163):

1. If `is_view_state(command)` — `SelectTimeline` / `MoveTimeline` (mod.rs:832-837) — apply without
   touching history, and return. Q5 asks whether these stay in the variant at all.
2. `apply` into the project while filling an `UndoRecord`.
3. On error, discard the record; history is untouched (editor.rs:121-123).
4. If `!outcome.applied`, discard the record; nothing is recorded (editor.rs:143-145).
5. Run the derived steps (`tidy_touched`, `follow_first_hdr`) as part of the same step and extend
   the record with anything they write, so undo takes them back with the edit.
6. Coalesce: if `gesture` is non-null and equals the last entry's gesture and no other step
   intervened, **discard the new record and keep the earlier one**, so undo lands where the gesture
   began (editor.rs:148-160). Otherwise push a new entry.
7. Clear `redo_`.

`end_gesture()` clears the last entry's gesture (editor.rs:253-257). Concat has no time-based
gesture timeout — the caller decides when a drag ends; Q2 asks whether the host adds one.

### 2.4 Dirty tracking

`Editor::dirty()` is true when a change has occurred since the last save. Track it as a saved
counter/token: `History` bumps a monotonically increasing `generation_` on every recorded step and
on every view-state edit (view state is saved but not undoable, editor.rs:829-837); the editor
stores the generation at save time. `dirty = generation_ != saved_generation_`. This distinguishes
"undo back to the saved point" from "never saved", which a bare `can_undo` cannot.

---

## 3. Rational time at edit points

### 3.1 Where the boundary is

`docs/architecture.md` §7: rational time everywhere, no float seconds for edit points. The host
already has `genesis::core::Rational`, `FrameRate`, `TimeRange` (`src/core/ClipTimeline.h:30,107,151`)
and `Rational::approximate(double)` is documented as "the seam where a UI's double becomes exact"
(`ClipTimeline.h:46-50`).

**Time-valued fields in the project model become `Rational`.** These are seconds:

| Type | Field | Host line |
|---|---|---|
| `Clip` | `start` | `ModelTypes.h:993` |
| `Clip` | `duration` | `ModelTypes.h:996` |
| `Clip` | `source_start` | `ModelTypes.h:998` |
| `Clip` | `fade_in`, `fade_out` | `ModelTypes.h:1001-1003` |
| `Transition` | `duration` | `ModelTypes.h:844` |
| `Stroke` | `at` (source instant) | `ModelTypes.h:689` |
| `MediaItem` | `duration` (probe metadata used to seed clip length) | `ModelTypes.h:308` |

**Fields that stay `double`** because they are dimensionless, not seconds: the key/param fractions
`ClipKey::at`, `ParamKey::at`, `SpeedPoint::at` (fractions of clip length); `SpeedPoint::speed`;
every transform, blend, ease, colour and opacity scalar; `VideoSettings.rate_num/rate_den` is already
exact (`ModelTypes.h:1515-1517`). Rationalising the fractions is optional future work, not this
slice; it changes no semantics and widens the diff across `rewindow_keys`/`absorb_keys`.

`MediaItem::duration` is the one judgement call: it is probe metadata, but it seeds a clip's
default duration (mod.rs:52-55, clips.rs:610-623). Recommend `std::optional<Rational>`, since the
value crosses into a clip; if the probe reports a decimal double the command boundary converts once
with `Rational::approximate`. Q4 confirms.

### 3.2 The seam

The command/UI/API boundary accepts `double` seconds where a caller has one; the command layer
converts exactly once (`Rational::approximate`, or `Rational::parse` for a `"num/den"` string like
`MediaItem::frame_rate_fraction`, `ModelTypes.h:319`). No `double` is ever stored at an edit point,
and no `Rational` round-trips through `double` (`ClipTimeline.h:68-71`).

### 3.3 Arithmetic that must be re-derived over rationals

- `Clip::blank` minimum duration `1.0/60.0` (`ModelTypes.h:1079`) and `Clip::tidy`'s floor
  (`ModelTypes.h:1097`) become `Rational{1, 60}`.
- `split_source` (`clips.rs:706-708`): `source_start + offset * speed` — exact rational arithmetic.
- `rewindow_keys` / `absorb_keys` (`ModelTypes.h:1367,1452`): the stored keys are still fractions
  (`double`), but `old`/`from`/`to` are `Rational`; the fraction conversion is the single documented
  conversion point, and `KEY_EPSILON` (`ModelTypes.h:268`) stays a fraction tolerance.
- `Timeline::duration()` (`ModelTypes.h:1632-1638`) and `Timeline::duration` in the core type
  (`ClipTimeline.h:499`) become exact.
- Trim (`clips.rs:178-249`), `close_gaps` (`clips.rs:578-608`), ripple room (`clips.rs:630-655`) and
  `JOIN_EPSILON` (`clips.rs:568`) comparisons become exact integer comparisons on rationals (or a
  rational epsilon). This is the "keys stay on their instant of the picture" correctness hotspot
  (rewrite-plan §3(d)).
- `SetClipSpeed` / `SetClipSpeedCurve` duration = `source_covered / speed`
  (`properties.rs:90-111,270-292`): rational division; `SpeedCurve::mean` returns a `double`, so the
  mean becomes the one place a rational is approximated — Q4 asks whether `SpeedCurve::mean` should
  gain a rational return.

### 3.4 Change now or later

**Recommendation: change the model's time fields now.**

- The serializer does not exist. `doc.rs` is unported, `DOCUMENT_VERSION`/`MIGRATIONS`
  (`concat-project/src/doc.rs:41,62`) have no host counterpart, and the model's `extra` maps are
  explicitly not ported (`ModelTypes.h:352-353,397-398,1064-1065,1576-1577,1680-1681`). There is no
  document format to migrate and no external caller of the command API.
- Cost now: seven field type changes, the functions in §3.3, and the command arms that read them.
- Cost later: a JSON migration under `docs/rewrite-plan.md` §7 Q3, a compatibility proof for every
  file written before the change, plus re-touching the same functions. Strictly larger.
- Risk of waiting: every test written against `f64` seconds must be rewritten, and float equality
  bugs can be baked into fixtures.

---

## 4. Command inventory

Concat's `Command` enum has **43 variants** (`concat-project/src/commands/mod.rs:231-688`), split
across `clips.rs`, `properties.rs`, `media.rs`, `timelines.rs`, `audio.rs`, `tracks.rs`, plus the
`Batch` composition in `mod.rs`.

**Tally: port-as-is 26, re-derive 15, drop 2** (view-state demoted). `speed.rs` adds two shared
helpers, both port-as-is.

### 4.1 Port-as-is (26)

Control flow and arithmetic unchanged; only the time type (§3) or a shared helper moves.

| Verb | Concat | Notes |
|---|---|---|
| `AddMedia` | media.rs:18-51 | path-dedup; mint `m` |
| `RemoveMedia` | media.rs:132-147 | sweeps clips on all timelines |
| `SetMediaPlaceholder` | media.rs:53-66 | flag toggle |
| `FillSlot` | media.rs:68-130 | in-place swap, all timelines |
| `AddFont` | media.rs:149-158 | path-dedup |
| `RemoveFont` | media.rs:160-170 | titles keep the family |
| `UpdateMediaPath` | media.rs:172-182 | relink |
| `SetMediaColorRange` | media.rs:184-194 | model field; engine read is adapter (§4.4) |
| `AddTrack` | tracks.rs:18-31 | |
| `RemoveTrack` | tracks.rs:33-48 | floor of one |
| `SetTrackFlag` | tracks.rs:50-68 | |
| `AddTimeline` | timelines.rs:18-52 | four fresh lanes |
| `RemoveTimeline` | timelines.rs:69-93 | floor of one |
| `SetTimelineVideo` | timelines.rs:54-67 | `is_sane` refusal, no clamp |
| `RenameTimeline` | timelines.rs:95-111 | trim, refuse blank |
| `UpdateClip` | properties.rs:18-88 | the `ClipPatch` arms |
| `SetClipTransform` | properties.rs:294-330 | scalar clamps |
| `SetClipCutout` | properties.rs:113-123 | `Cutout::tidy` |
| `AddCutoutStroke` | properties.rs:125-140 | `Stroke::tidy`; `Stroke::at` becomes Rational |
| `SetClipKey` | properties.rs:142-175 | per-property clamps |
| `ClearClipKey` | properties.rs:177-191 | |
| `ClearClipKeys` | properties.rs:193-203 | |
| `SetEffectKey` | properties.rs:205-229 | |
| `ClearEffectKey` | properties.rs:231-249 | |
| `ClearEffectKeys` | properties.rs:251-268 | |
| `Batch` | mod.rs:266-269, 990-1009 | composition re-derived for atomicity (§1.3); verb unchanged |

### 4.2 Re-derive (15)

The verb survives; its arithmetic or its engine coupling changes.

| Verb | Concat | Why re-derived |
|---|---|---|
| `AddClip` | clips.rs:18-45 | rational start/duration; ripple room (clips.rs:630) exact |
| `AddClipAtFirstFree` | clips.rs:47-67 | rational placement; free-lane predicate exact |
| `AddTextClip` | clips.rs:69-120 | rational duration/offset; Qt text is a later `replace` |
| `AddLayerClip` | clips.rs:122-154 | rational duration; effect id from the Pack model |
| `MoveClips` | clips.rs:156-176 | rational `start` |
| `TrimClip` | clips.rs:178-249 | rational head/tail, in-point, rewindow, ripple; exact edge |
| `SplitClips` | clips.rs:251-306 | exact `split_source`; rewindow over rationals |
| `MergeClips` | clips.rs:495-530 | exact touch test (`JOIN_EPSILON`); rewindow + absorb |
| `FreezeFrame` | clips.rs:362-493 | split + ripple + still; still extraction is adapter work |
| `ReplaceClipMedia` | clips.rs:308-360 | probe/adapter coupling; rational in-point |
| `RemoveClips` | clips.rs:532-561 | exact `close_gaps` (clips.rs:578) |
| `SetClipSpeed` | properties.rs:90-111 | rational duration from held source coverage |
| `SetClipSpeedCurve` | properties.rs:270-292 | curve mean → rational duration (Q4) |
| `DetachAudio` | audio.rs:18-108 | GES sound semantics; multi-track naming |
| `ReattachAudio` | audio.rs:110-143 | GES sound semantics |

### 4.3 Drop (2)

| Verb | Concat | Verdict |
|---|---|---|
| `SelectTimeline` | timelines.rs:113-125 | **drop from the recorded stack**; view state, saved not undoable (mod.rs:832-837, editor.rs:138-140) |
| `MoveTimeline` | timelines.rs:127-145 | **drop from the recorded stack**; view state |

Q5 asks whether these leave the `Command` variant entirely or stay as non-undoable members.

### 4.4 `speed.rs`

`speed.rs` is two shared helpers, not commands: `curve_of` (speed.rs:14-20) and `mean_of`
(speed.rs:24-26). Both are **port-as-is**: `curve_of` is already `SpeedCurve::create`
(`src/core/SpeedCurve.h:40`), and `mean_of` is `SpeedCurve::mean` (`SpeedCurve.h:56`). The command
layer calls the core helper; no new speed code exists in this layer. Q4 covers whether `mean`
returns a rational.

### 4.5 Engine-coupled verbs and the host boundary

`FreezeFrame` (still jpg), `ReplaceClipMedia` (enhanced/reversed copy), `DetachAudio`/`ReattachAudio`
(sound extraction), and `SetMediaColorRange` (reading levels) depend on probe/extract work that is
Phase 2/3 adapter and engine work (R5). The **command and model shapes** port here; the media work
does not. A command that needs a probe artifact takes the already-probed `NewMedia` as an argument
(`FreezeFrame.still`, mod.rs:516-518), so the layer stays engine-free.

---

## 5. Serialization seam

`concat-project/src/doc.rs` is the document layer: migrations (`doc.rs:62-71`), `DOCUMENT_VERSION`
(`doc.rs:41`), tolerant parse into `extra` maps (`doc.rs:170-174`), settle (`doc.rs:178-216`) and
write with the flat-mirror fields (`doc.rs:314-361`). It is **not** ported by this plan and **not
designed here**.

The seam is marked, not opened:

- The model already reserves the `extra` maps this layer owns (`ModelTypes.h:352-353,397-398,
  1064-1065,1576-1577,1680-1681`). No other file may use them.
- The planned host file is `src/project/Document.h` / `Document.cpp`, created **only when
  `docs/rewrite-plan.md` §7 Q3 is answered**. If Q3 is "read `concat.json` one-way", the layer adds
  Concat's migration table; if Q3 is "new format", the command/`extra` wire format is free.
- Nothing in the command or undo layer serializes. `Command` being a tagged variant (§1.1) keeps
  the option open without paying for it now.

---

## 6. File layout

All under `src/project/`, namespace `genesis::project`, standard C++23, no Qt, no GStreamer.

| File | Contents | Depends on |
|---|---|---|
| `ModelTypes.h` (exists) | project model; time fields become `Rational` (§3) | `core/Animation.h`, + `core/ClipTimeline.h` for `Rational` |
| `IdMinter.h` / `.cpp` | counter, `next`, `adopt`, `adopt_project` (mod.rs:752-795) | none |
| `CommandTypes.h` | `Command` variant, `NewMedia`, `ClipPatch`, `ClipMove`, `TrimEdge`, `TrackFlag`, `Outcome`, `CommandError`, `is_view_state`, `has_non_finite` | `ModelTypes.h`, `IdMinter.h` |
| `commands/Apply.h` / `Apply.cpp` | `apply` dispatcher; non-finite guard; `Batch` atomicity | `CommandTypes.h`, verb groups |
| `commands/ClipPlacement.cpp` and `commands/ClipReshape.cpp` | placing/moving/cutting verbs | `commands/Apply.h` |
| `commands/Properties.cpp` | patch/speed/transform/keys/cutout verbs | `commands/Apply.h` |
| `commands/Media.cpp` | bin, slots, fonts, relink, colour range | `commands/Apply.h` |
| `commands/Timelines.cpp` | timeline verbs | `commands/Apply.h` |
| `commands/Audio.cpp` | detach/reattach | `commands/Apply.h` |
| `commands/Tracks.cpp` | lane verbs | `commands/Apply.h` |
| `Undo.h` / `Undo.cpp` | `UndoRecord`, `*Snapshot`, `History`, `HistoryEntry`, coalescing | `ModelTypes.h`, `CommandTypes.h` |
| `Editor.h` / `.cpp` | owns `Project`, `IdMinter`, `History`; `apply`, `apply_within`, `undo`, `redo`, `end_gesture`, `tidy_touched`, `follow_first_hdr`, dirty token | all of the above |
| `Document.h` / `.cpp` | serialization — **seam only**, §5, gated on Q3 | `ModelTypes.h` |

Dependency order: `IdMinter` → `CommandTypes` → `commands/*` → `Undo` → `Editor`; `Document` last and
optional. `SpeedCurve` comes from `core/SpeedCurve.h`, not a new file. No catch-all `utils`/`helpers`
file; every file above has a single named responsibility.

---

## 7. Verification strategy

The whole layer is engine-independent: it links `src/core/*.cpp` and `src/project/*.cpp` only. No
GES, no Qt, no media. This is the Phase 2 acceptance requirement ("adapter contract is interface
only; no GES/MLT type appears", rewrite-plan §4 Phase 1) made testable before any adapter exists.

Planned tests under `tests/project/`, using the existing no-framework `check`/`check_near` style
(`tests/CoreTests.cpp:19-37`):

1. **Per-verb behavior** — mirror `lib.rs`'s test list (`lib.rs:35-3791`): clamps, tolerated
   no-ops with `applied == false` (lib.rs:2878, 2901), batch atomicity (lib.rs:2506, 2532), ripple
   delete (lib.rs:292), magnetic trim (lib.rs:485), split/merge continuity (lib.rs:998), freeze
   (lib.rs:876), detach/reattach (lib.rs:1851, 1880), timeline floors (lib.rs:1995), fonts
   (lib.rs:3383), NaN refusal (lib.rs:1106).
2. **Undo exactness** — the acceptance bar: for every verb and a random command sequence,
   `undo(apply(cmd)) == before` and `redo(undo) == after`. Compare whole `Project` with
   `operator==` (`ModelTypes.h:1752`).
3. **Coupled and derived steps** — undo of a trim/split/merge restores keys on their exact
   instants (lib.rs:1549); a first HDR clip and its timeline promotion undo together (lib.rs:144);
   `tidy` round-trips (lib.rs:1355).
4. **Gesture coalescing and view-state exclusion** — a named gesture is one step (lib.rs:1230);
   switching/reordering tabs records nothing (lib.rs:1275); depth-200 eviction (lib.rs:2977).
5. **Rational time** — edit points round-trip without a `double`; frame-grid equality at 30000/1001;
   `1/60` floors exact; `rewindow_keys` keeps the instant invariant.
6. **Id minting** — restored ids are never re-issued (lib.rs:2236); undo/redo keep ids stable.

Build check: `g++ -std=c++23 -Wall -Wextra -Wpedantic -Werror -I src src/core/*.cpp
src/project/*.cpp tests/project/*.cpp`. One parse plus one test run is the evidence per phase; the
model layer is already proven to compile under the same flags.

---

## 8. Open questions

Each blocks a named decision.

1. **Fidelity target** (rewrite-plan §7 Q2). Exact Concat undo semantics — gesture granularity,
   `Clip::tidy` clamps, HDR coupling — or Genesis-0 rules? Blocks the §7 undo acceptance tests.
2. **Depth and gesture timeout** (rewrite-plan §3(c)). Keep depth 200 and caller-driven gestures,
   or set a host depth/timeout? Blocks `History` constants (§2.1) and `end_gesture` (§2.3).
3. **Project-file compatibility** (rewrite-plan §7 Q3). Read/migrate `concat.json`, or a new format?
   Blocks the §5 seam and whether the command wire format must match Concat's.
4. **Rational migration timing.** Confirm changing `ModelTypes.h` time fields now, with no
   serializer, rather than keeping `f64` until Q3. Also: does `MediaItem::duration` and
   `SpeedCurve::mean` become rational? Blocks §3 and the field diff.
5. **View-state commands.** Do `SelectTimeline`/`MoveTimeline` stay in the `Command` variant as
   non-undoable members, or move to a separate view-state type? Blocks `CommandTypes.h` shape.
6. **Inverse representation.** Accept scoped before-image `UndoRecord`s, or require true mirrored
   inverse commands for structural edits? Blocks §2's acceptance and the `Undo.h` API.
7. **Template slots.** Are `SetMediaPlaceholder`/`FillSlot` in scope now (product templates) or
   deferred? Blocks their inventory verdict (§4.1).
8. **Deferred features.** Text (Qt rasteriser is a later `replace`) and cutout/vision (concat-vision
   is deferred in rewrite-plan §2). Ship the inert model commands now, or defer them with their
   features? Blocks the inventory's completeness.

---

## Appendix — citation index

Concat, read-only at `/home/bigbrother/Projects/video-editors/concat`:

- `concat-project/src/commands/mod.rs` — `Command` enum 231-688; `ClipMove` 69-78; `ClipPatch`
  85-153; `NewMedia` 179-218; `Outcome` 694-706; `CommandError` 713-746; `IdMinter` 752-795;
  `next_numbered` 815-826; `first_line` 798-810; `is_view_state` 832-837; `has_non_finite` 843-961;
  `assign` 968-975; `apply` 981-1054.
- `concat-project/src/commands/clips.rs` — 18, 47, 69, 122, 156, 178, 251, 308, 362, 495, 532, 568,
  578, 610, 630, 659, 680, 706, 712.
- `concat-project/src/commands/properties.rs` — 18, 90, 113, 125, 142, 177, 193, 205, 231, 251, 270,
  294.
- `concat-project/src/commands/media.rs` — 18, 53, 68, 132, 149, 160, 172, 184.
- `concat-project/src/commands/timelines.rs` — 18, 54, 69, 95, 113, 127.
- `concat-project/src/commands/audio.rs` — 18, 110.
- `concat-project/src/commands/tracks.rs` — 18, 33, 50.
- `concat-project/src/editor.rs` — 32, 35-52, 109-123, 133-163, 170-197, 205-247, 253-284.
- `concat-project/src/doc.rs` — 41, 62, 170, 178, 314.
- `concat-project/src/speed.rs` — 14, 24.
- `concat-project/src/model.rs` — 1662, 1821.
- `concat-project/src/lib.rs` — re-exports 24-33; tests 35 onward.

Host:

- `src/project/ModelTypes.h` — time fields 308, 689, 844, 993-1003; `Clip::blank` 1077;
  `Clip::tidy` 1096-1204; `reindow_keys` 1367; `absorb_keys` 1452; `Timeline` 1560; `Project` 1669.
- `src/core/ClipTimeline.h` — `Rational` 30, `approximate` 50, `FrameRate` 107, `TimeRange` 151.
- `src/core/SpeedCurve.h` — `create` 40, `mean` 56.
- `src/core/Animation.h` — `Ease`, `Key`, `KeyTrack`.
