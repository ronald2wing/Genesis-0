// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Clip commands: placing, trimming, moving, splitting and rearranging clips.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/ClipTimeline.h"
#include "project/model/Cutout.h"
#include "project/model/Effects.h"
#include "project/model/Speed.h"
#include "project/model/Text.h"

#include "project/command/MediaCommands.h"

namespace genesis::project {

using genesis::core::Rational;

// Which end of a clip a trim drags. The two are not symmetric: see
// Command::TrimClip.
// On disk: lowercase.
enum class TrimEdge {
    // The head. Trimming here moves the in-point with the edge, so the
    // remaining frames stay where they were on the timeline.
    Start,
    // The tail. Trimming here only lengthens or shortens the clip.
    End,
};

// Where one clip is going, in a multi-clip move.
// On disk: camelCase.
struct ClipMove
{
    // The clip to move. An unknown id is skipped, not an error - the rest
    // of the batch of moves still lands.
    std::string clip_id;
    // New timeline position in seconds, floored at 0.
    Rational start = Rational::ZERO;
    // Destination track. An unknown id moves the clip in time but leaves it
    // on its current track.
    std::string track_id;

    bool operator==(const ClipMove &) const = default;
};

// A partial update to one clip. Every field optional; `transition_in` and
// `text` are double-optional so "clear it" and "leave it alone" stay
// distinct on the wire (absent = untouched, null = cleared).
// On disk: camelCase.
struct ClipPatch
{
    // New display name, taken verbatim. A later `text` patch overwrites it
    // with the title's first line.
    std::optional<std::string> name;
    // New gain; floored at 0, deliberately not capped at 1 - boosting quiet
    // footage is legitimate.
    std::optional<double> volume;
    // New fade-in length in seconds, floored at 0.
    std::optional<Rational> fade_in;
    // New fade-out length in seconds, floored at 0.
    std::optional<Rational> fade_out;
    // New opacity, clamped into 0..=1.
    std::optional<double> opacity;
    // New pitch-preservation setting, taken as sent.
    std::optional<bool> preserve_pitch;
    // Whether the clip's own sound is silenced: true mutes it, false lets
    // it play again. A detach sets this on the video it took the sound
    // from, so this is also how a video whose detached sound was later
    // deleted gets its voice back.
    // On disk: default.
    std::optional<bool> muted;
    // Mirror left to right.
    // On disk: default.
    std::optional<bool> flip_h;
    // Mirror top to bottom.
    // On disk: default.
    std::optional<bool> flip_v;
    // The blend mode's name; empty is normal.
    // On disk: default.
    std::optional<std::string> blend;
    // The crop, same three-way wire semantics as `transition_in`: absent
    // leaves it alone, null takes it off, a value replaces it.
    // On disk: default; absent = untouched, null = cleared.
    std::optional<std::optional<Crop>> crop;
    // Wholesale replacement of the audio filter chain - the UI sends the
    // full list, not a diff.
    std::optional<std::vector<AppliedFilter>> filters;
    // Wholesale replacement of the video effect chain, like `filters`.
    std::optional<std::vector<AppliedFilter>> video_effects;
    // The transition on the cut into the clip: absent leaves it alone,
    // null clears it, a value replaces it.
    // On disk: default; absent = untouched, null = cleared.
    std::optional<std::optional<Transition>> transition_in;
    // The title styling, same three-way wire semantics as `transition_in`.
    // Setting a style also renames the clip after its first line.
    // On disk: default; absent = untouched, null = cleared.
    std::optional<std::optional<TextStyle>> text;
    // Which of the media's audio streams the clip plays, by stream index:
    // absent leaves it alone, null goes back to the file's first, a value
    // names one. See Clip::audio_stream.
    // On disk: default; absent = untouched, null = cleared.
    std::optional<std::optional<std::uint32_t>> audio_stream;

    bool operator==(const ClipPatch &) const = default;
};

// Places a clip of `media_id` on a named track, minting a "c" id.
// Duration comes from the media (five seconds for a still or unknown
// length); errs if the media or track no longer exists.
struct AddClip
{
    // The bin item to cut from.
    std::string media_id;
    // The lane to place it on.
    std::string track_id;
    // Timeline position in seconds, floored at 0.
    Rational start = Rational::ZERO;
    // When true and the drop would overlap something on this track,
    // every clip at or after `start` is shifted right by the new
    // clip's duration, so the drop lands without covering anything.
    // False for programmatic adds, true for a drop from the bin.
    bool ripple = false;

    bool operator==(const AddClip &) const = default;
};

// Command::AddClip without naming a lane: lands on the lowest
// track with nothing in the clip's span, falling back to the bottom
// track (overlap and all) rather than refusing.
struct AddClipAtFirstFree
{
    // The bin item to cut from.
    std::string media_id;
    // Timeline position in seconds, floored at 0.
    Rational start = Rational::ZERO;

    bool operator==(const AddClipAtFirstFree &) const = default;
};

// Places a title: a clip with no media behind it, named after the
// text's first line, minting a "c" id.
struct AddTextClip
{
    // The lane to place it on. None picks the first free track like
    // Command::AddClipAtFirstFree; naming a vanished track errs.
    std::optional<std::string> track_id;
    // When `track_id` is None and this is true, the clip lands on the
    // first free lane *above* the highest one occupied over its span,
    // minting a new lane at the top when every one above is taken.
    // What the editor sets for a caption and for a title alike, so
    // words sit over the video rather than under it; false keeps the
    // bottom-first walk of Command::AddClipAtFirstFree. Ignored
    // when `track_id` names a track.
    bool above = false;
    // Timeline position in seconds, floored at 0.
    Rational start = Rational::ZERO;
    // The words and their look. None means TextStyle::default.
    std::optional<TextStyle> style;
    // Seconds on the timeline; the editorial default when absent. Here
    // so a caption run can land as one batch instead of add-then-trim
    // per clip - a batch cannot trim a clip whose id it cannot know yet.
    std::optional<Rational> duration;
    // Vertical placement as a frame-height fraction, clamped like
    // SetClipTransform. Lower thirds are made of this.
    std::optional<double> offset_y;

    bool operator==(const AddTextClip &) const = default;
};

// Places a layer: a look or an effect over a span of the timeline that
// treats everything beneath it. No media; the chain starts as the one
// package named, at its defaults, and the clip's opacity is how hard it
// is applied.
struct AddLayerClip
{
    // The lane to place it on; None picks the first free track.
    std::optional<std::string> track_id;
    // Timeline position in seconds, floored at 0.
    Rational start = Rational::ZERO;
    // Seconds on the timeline; the editorial default when absent.
    std::optional<Rational> duration;
    // The package the layer applies.
    std::string effect_id;
    // What the lane calls it.
    std::string name;

    bool operator==(const AddLayerClip &) const = default;
};

// Repositions any number of clips in one edit - one undo step for a
// whole multi-selection drag. Unknown clips and tracks are tolerated
// per ClipMove.
struct MoveClips
{
    // Where each clip is going.
    std::vector<ClipMove> moves;

    bool operator==(const MoveClips &) const = default;
};

// Drags one edge of a clip. A head trim moves the in-point with the
// edge (scaled by speed) so the remaining pixels do not slide; either
// edge stops at the sixtieth-of-a-second minimum duration. An unknown
// clip is a no-op.
struct TrimClip
{
    // The clip to trim.
    std::string clip_id;
    // Which edge is being dragged.
    TrimEdge edge;
    // Signed seconds of timeline the edge moves: positive drags the
    // head right (shortening) or the tail right (lengthening).
    Rational delta = Rational::ZERO;
    // When true the lane closes up behind the trim - the magnetic
    // timeline. A tail trim moves every later clip on the track by
    // the change in length. A head trim keeps the clip where it was
    // (the in-point moves, the start does not) and moves every later
    // clip by what was cut or restored, so the trimmed clip and the
    // one behind it stay touching. False trims the clip alone.
    bool ripple = false;

    bool operator==(const TrimClip &) const = default;
};

// Cuts each named clip in two at one playhead time. The head keeps the
// id and the transition; the tail is minted fresh and stays
// source-continuous. A clip the time misses (or grazes within the
// minimum duration) is skipped.
struct SplitClips
{
    // The clips under the playhead - normally the selection.
    std::vector<std::string> clip_ids;
    // The cut point, in timeline seconds.
    Rational time = Rational::ZERO;

    bool operator==(const SplitClips &) const = default;
};

// Points a clip at another file - the enhanced or reversed copy of
// its media - adding the file to the bin first when it is not there.
// The clip's length, looks and name are kept, and its in-point unless
// `source_start` moves it: an enhanced copy stands in for the original
// frame for frame, a reversed one covers the span the clip showed and
// starts at its own zero. The bin keeps the original, since other
// clips may show it and undo may want it back. An unknown clip is a
// no-op.
struct ReplaceClipMedia
{
    // The clip to re-point.
    std::string clip_id;
    // The probed copy, as described by the host.
    NewMedia item;
    // A new in-point in the copy, in seconds, floored at 0. Absent
    // keeps the clip's.
    std::optional<Rational> source_start;

    bool operator==(const ReplaceClipMedia &) const = default;
};

// A freeze frame at `time`: splits `clip_id`, inserts a still of
// `duration` on the same track, and ripples later clips on that track
// by `duration`. Video needs a probed `still` (host-extracted jpg);
// image clips may omit it and reuse their media. Audio and text are
// no-ops. `created_id` is the freeze clip.
struct FreezeFrame
{
    // The picture clip under the playhead.
    std::string clip_id;
    // Timeline playhead; must fall strictly inside the clip.
    Rational time = Rational::ZERO;
    // Editorial length of the hold. Floored at MIN_CLIP_DURATION;
    // when absent or non-positive, uses DEFAULT_FREEZE_DURATION.
    std::optional<Rational> duration;
    // Probed still file. Required for video; ignored for image when
    // reusing the existing media.
    std::optional<NewMedia> still;

    bool operator==(const FreezeFrame &) const = default;
};

// Rejoins split pieces into the earliest piece, which keeps its id.
// Errs with a user-facing sentence (why_not_merge) unless the
// pieces sit on one track, come from one file at one speed, touch
// within a microsecond, and are still in source order.
struct MergeClips
{
    // The pieces to rejoin, any order.
    std::vector<std::string> clip_ids;

    bool operator==(const MergeClips &) const = default;
};

// Deletes clips from the active timeline. Unknown ids are ignored.
struct RemoveClips
{
    // The clips to delete.
    std::vector<std::string> clip_ids;
    // When true the deletion leaves no gap: on each track, every clip
    // that starts after a removed span moves left by the length of the
    // removed spans before it. A track the deletion never touched
    // stays where it is, so a picture going from one lane does not
    // pull the sound on another - the magnetic track, which is
    // what was asked for. False leaves the hole.
    bool ripple = false;

    bool operator==(const RemoveClips &) const = default;
};

} // namespace genesis::project
