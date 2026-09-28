// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-project/src/commands/clips.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "project/commands/Apply.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace genesis::project {
namespace {

// How far apart two clips may sit and still count as touching, in seconds.
const Rational JOIN_EPSILON = Rational{ 1, 1000000 };
// The floor on a clip's timeline length: a sixtieth of a second.
const Rational MIN_CLIP_DURATION = Rational{ 1, 60 };
// How long a freeze holds by default, when the command names none.
const Rational DEFAULT_FREEZE_DURATION = Rational{ 1, 1 };

// One removed span for the ripple of a delete: which track, and the
// `[start, end)` it occupied. `end` is `start + duration`.
using RemovedSpan = std::tuple<std::string, Rational, Rational>;

// The ripple of a delete: pulls every clip that sits after a removed
// span left by the length of the removed spans before it, on that clip's
// own track. Spans that overlap count once - a clip behind two doomed
// clips that shared five seconds moves by their union, not their sum, or
// it would land in front of what was in front of it. A span reaching past
// the clip's start counts only up to it, and nothing is pulled before
// zero.
// https://github.com/jub0t/Concat/issues/106
void close_gaps(Timeline &timeline, const std::vector<RemovedSpan> &removed, UndoRecord &undo)
{
    const auto behind_a_span = [&removed](const Clip &clip) {
        return std::any_of(removed.begin(), removed.end(), [&clip](const RemovedSpan &span) {
            return std::get<0>(span) == clip.track_id && std::get<1>(span) < clip.start;
        });
    };
    for (Clip *clip : timeline.clips_where(behind_a_span)) {
        std::vector<std::pair<Rational, Rational>> spans;
        for (const RemovedSpan &span : removed) {
            if (std::get<0>(span) == clip->track_id && std::get<1>(span) < clip->start) {
                spans.emplace_back(std::get<1>(span), std::min(std::get<2>(span), clip->start));
            }
        }
        if (spans.empty()) {
            continue;
        }
        std::sort(spans.begin(), spans.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });
        Rational gap = Rational::ZERO;
        Rational from = spans[0].first;
        Rational to = spans[0].second;
        for (std::size_t i = 1; i < spans.size(); ++i) {
            const Rational start = spans[i].first;
            const Rational end = spans[i].second;
            if (start <= to) {
                to = std::max(to, end);
            } else {
                gap = gap + (to - from);
                from = start;
                to = end;
            }
        }
        gap = gap + (to - from);
        record_before(undo, *clip);
        clip->start = std::max(clip->start - gap, Rational::ZERO);
    }
}

// Where each piece of a clip cut at `offset` begins in the source: the
// head keeps its in-point and the tail starts `offset × speed` later, so
// the two pieces together show exactly what the whole did.
std::pair<Rational, Rational> split_source(Rational source_start, double speed, Rational offset)
{
    return { source_start, source_start + offset * *Rational::approximate(speed) };
}

// Why these clips cannot be merged, or None if they can. A sentence, because
// a disabled button that will not say why is worse than no button.
std::optional<std::string> why_not_merge(const Timeline &timeline,
                                         const std::vector<std::string> &clip_ids)
{
    if (clip_ids.size() < 2) {
        return "Select two or more clips to merge.";
    }
    std::vector<const Clip *> clips;
    clips.reserve(clip_ids.size());
    for (const std::string &id : clip_ids) {
        const Clip *clip = timeline.clip(id);
        if (clip != nullptr) {
            clips.push_back(clip);
        }
    }
    if (clips.size() < 2) {
        return "Select two or more clips to merge.";
    }
    const Clip &first = *clips[0];
    if (std::any_of(clips.begin(), clips.end(),
                    [&first](const Clip *clip) { return clip->track_id != first.track_id; })) {
        return "Merged clips must be on the same track.";
    }
    if (std::any_of(clips.begin(), clips.end(),
                    [&first](const Clip *clip) { return clip->media_id != first.media_id; })) {
        return "Merged clips must come from the same file.";
    }
    if (std::any_of(clips.begin(), clips.end(),
                    [&first](const Clip *clip) { return clip->speed != first.speed; })) {
        return "Merged clips must play at the same speed.";
    }
    if (std::any_of(clips.begin(), clips.end(),
                    [&first](const Clip *clip) { return clip->kind != first.kind; })) {
        return "Merged clips must be the same kind.";
    }
    if (std::any_of(clips.begin(), clips.end(),
                    [](const Clip *clip) { return clip->speed_curve.has_value(); })) {
        return "A clip with a speed curve cannot be merged.";
    }
    if (std::any_of(clips.begin(), clips.end(), [&first](const Clip *clip) {
            return clip->audio_stream != first.audio_stream;
        })) {
        return "Merged clips must play the same audio track.";
    }

    std::vector<const Clip *> ordered = clips;
    std::sort(ordered.begin(), ordered.end(),
              [](const Clip *a, const Clip *b) { return a->start < b->start; });
    for (std::size_t i = 1; i < ordered.size(); ++i) {
        const Clip &previous = *ordered[i - 1];
        const Clip &current = *ordered[i];
        Rational gap = current.start - (previous.start + previous.duration);
        if (gap.is_negative()) {
            gap = -gap;
        }
        if (gap > JOIN_EPSILON) {
            return "Merged clips must touch, with no gap or overlap.";
        }
        // The next piece starts where the last one's source ended.
        Rational continuous = current.source_start
                - (previous.source_start
                   + previous.duration * *Rational::approximate(previous.speed));
        if (continuous.is_negative()) {
            continuous = -continuous;
        }
        if (continuous > JOIN_EPSILON) {
            return "These pieces are no longer in their original order.";
        }
    }
    return std::nullopt;
}

} // namespace

// Reshaping clips: the edits that change a clip's own span, its source or its
// neighbours - trimming, cutting, merging, freezing, re-pointing and removing.
//
// One arm per command, exactly as `apply_clips` routes them here; everything
// these arms share lives in the parent module.
ApplyResult apply_clip_reshape(const Command &command, Project &project, IdMinter &mint,
                               UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, TrimClip>) {
                    // Drags one edge. The head moves the in-point with it, so the
                    // pixels under the remaining clip do not slide; either edge
                    // stops at the sixtieth-of-a-second minimum.
                    Timeline &timeline = project.active_mut();
                    Clip *clip = timeline.clip_mut(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *clip);
                    const std::string track_id = clip->track_id;
                    const Rational anchor = clip->start;
                    const Rational old_end = clip->start + clip->duration;
                    // What the trim did, and what the lane behind it does about
                    // it when magnetic: `by` is how far the later clips move,
                    // `behind` where "later" begins.
                    Rational by = Rational::ZERO;
                    Rational behind = Rational::ZERO;
                    bool applied = false;
                    if (alternative.edge == TrimEdge::End) {
                        const Rational duration =
                                std::max(clip->duration + alternative.delta, MIN_CLIP_DURATION);
                        const Rational old = clip->duration;
                        applied = assign(clip->duration, duration);
                        if (applied) {
                            clip->rewindow_keys(old, Rational::ZERO, duration);
                        }
                        by = duration - old;
                        behind = old_end - JOIN_EPSILON;
                    } else {
                        // Dragging the head moves the in-point too, so the pixels
                        // under the remaining part of the clip do not slide. The
                        // head cannot reach before the source begins.
                        Rational shift =
                                std::min(alternative.delta, clip->duration - MIN_CLIP_DURATION);
                        shift = std::max(
                                shift, (-clip->source_start) / *Rational::approximate(clip->speed));
                        // A magnetic head trim never moves the clip, so the
                        // timeline's own start is no limit to it; a plain one
                        // stops at zero.
                        Rational start = clip->start + shift;
                        if (!alternative.ripple) {
                            start = std::max(start, Rational::ZERO);
                        }
                        const Rational moved = start - clip->start;
                        const Rational duration = clip->duration - moved;
                        const Rational source_start = std::max(
                                clip->source_start + moved * *Rational::approximate(clip->speed),
                                Rational::ZERO);
                        const Rational old = clip->duration;
                        // Bitwise so no assignment is short-circuited away.
                        applied |= assign(clip->start, start);
                        applied |= assign(clip->duration, duration);
                        applied |= assign(clip->source_start, source_start);
                        if (applied) {
                            clip->rewindow_keys(old, moved, old);
                        }
                        if (alternative.ripple) {
                            // The in-point moved; the clip stays put, and the
                            // lane behind it closes by what came off the head.
                            clip->start = anchor;
                        }
                        by = -moved;
                        behind = anchor + JOIN_EPSILON;
                    }
                    if (alternative.ripple && applied && by != Rational::ZERO) {
                        for (Clip *other : timeline.clips_where([&](const Clip &c) {
                                 return c.id != alternative.clip_id && c.track_id == track_id
                                         && c.start >= behind;
                             })) {
                            record_before(undo, *other);
                            other->start = std::max(other->start + by, Rational::ZERO);
                        }
                    }
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SplitClips>) {
                    // Cuts each named clip in two at one playhead time. The head
                    // keeps the id and the transition; the tail is minted fresh
                    // and stays source-continuous. A clip the time misses (or
                    // grazes within the minimum duration) is skipped.
                    Timeline &timeline = project.active_mut();
                    std::optional<std::string> created;
                    for (const std::string &clip_id : alternative.clip_ids) {
                        const auto it = std::find_if(
                                timeline.clips.begin(), timeline.clips.end(),
                                [&clip_id](const Clip &clip) { return clip.id == clip_id; });
                        if (it == timeline.clips.end()) {
                            continue;
                        }
                        const std::size_t index =
                                static_cast<std::size_t>(it - timeline.clips.begin());
                        record_before(undo, timeline.clips[index]);
                        {
                            // A curve does not survive a cut in halves: the map
                            // from here to the source is not affine, so both
                            // halves go to the constant mean, which is what they
                            // averaged.
                            Clip &clip = timeline.clip_at_mut(index);
                            const Rational offset = alternative.time - clip.start;
                            if (offset > MIN_CLIP_DURATION
                                && offset < clip.duration - MIN_CLIP_DURATION
                                && clip.speed_curve.has_value()) {
                                clip.speed_curve.reset();
                            }
                        }
                        const Clip &clip = timeline.clips[index];
                        const Rational offset = alternative.time - clip.start;
                        if (offset <= MIN_CLIP_DURATION
                            || offset >= clip.duration - MIN_CLIP_DURATION) {
                            continue;
                        }
                        const auto [head_source, tail_source] =
                                split_source(clip.source_start, clip.speed, offset);
                        const Rational whole = clip.duration;
                        Clip tail = clip;
                        tail.id = mint.next("c");
                        tail.start = clip.start + offset;
                        tail.duration = clip.duration - offset;
                        tail.source_start = tail_source;
                        // The transition belongs to the cut at the original
                        // clip's start, which the head keeps; the way in belongs
                        // to the head and the way out to the tail, so neither
                        // piece plays an entrance or an exit the whole did not
                        // have at the cut.
                        tail.transition_in = std::nullopt;
                        tail.fade_in = Rational::ZERO;
                        tail.rewindow_keys(whole, offset, whole);
                        const std::string tail_id = tail.id;
                        record_created(undo, tail_id, std::in_place_type<Clip>);
                        Clip &head = timeline.clip_at_mut(index);
                        head.duration = offset;
                        head.source_start = head_source;
                        head.fade_out = Rational::ZERO;
                        head.rewindow_keys(whole, Rational::ZERO, offset);
                        timeline.clips.insert(timeline.clips.begin() + index + 1, std::move(tail));
                        created = tail_id;
                    }
                    // A split always mints the tail, so "minted anything" and
                    // "changed anything" are the same fact here.
                    return Outcome{ created, created.has_value() };
                }

                if constexpr (std::is_same_v<T, ReplaceClipMedia>) {
                    // Points a clip at another file, adding the file to the bin
                    // first when it is not there. An unknown clip is a no-op.
                    if (project.active().clip(alternative.clip_id) == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    // The copy's bin entry, or the one already there for its
                    // path: two clips of one file enhanced in turn share one
                    // copy.
                    std::string media_id;
                    const auto existing =
                            std::find_if(project.media.begin(), project.media.end(),
                                         [&alternative](const MediaItem &media) {
                                             return media.path == alternative.item.path;
                                         });
                    if (existing != project.media.end()) {
                        media_id = existing->id;
                    } else {
                        media_id = mint.next("m");
                        MediaItem media;
                        media.id = media_id;
                        media.path = alternative.item.path;
                        media.name = alternative.item.name;
                        media.duration = alternative.item.duration;
                        media.kind = alternative.item.kind;
                        media.width = alternative.item.width;
                        media.height = alternative.item.height;
                        media.frame_rate = alternative.item.frame_rate;
                        media.frame_rate_fraction = alternative.item.frame_rate_fraction;
                        media.video_codec = alternative.item.video_codec;
                        media.audio_codec = alternative.item.audio_codec;
                        media.has_audio = alternative.item.has_audio;
                        media.audio_tracks = alternative.item.audio_tracks;
                        media.origin = alternative.item.origin;
                        media.placeholder = false;
                        media.color_range = std::nullopt;
                        media.color_space = alternative.item.color_space;
                        project.media.push_back(std::move(media));
                        record_created(undo, media_id, std::in_place_type<MediaItem>);
                    }
                    Timeline &timeline = project.active_mut();
                    const auto it = std::find_if(timeline.clips.begin(), timeline.clips.end(),
                                                 [&alternative](const Clip &clip) {
                                                     return clip.id == alternative.clip_id;
                                                 });
                    if (it == timeline.clips.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    // Bitwise so no assignment is short-circuited away.
                    bool applied = assign(it->media_id, media_id);
                    if (alternative.source_start) {
                        applied |= assign(it->source_start,
                                          std::max(*alternative.source_start, Rational::ZERO));
                    }
                    if (!applied) {
                        return Outcome{ std::nullopt, false };
                    }
                    return Outcome{ media_id, true };
                }

                if constexpr (std::is_same_v<T, FreezeFrame>) {
                    // Holds a picture at `time`: splits the clip, slots a still
                    // between the pieces, and ripples everything later on the
                    // track right by the hold. A video needs a probed still; an
                    // image may reuse its own media. Audio and text are no-ops.
                    const Rational hold =
                            std::max(alternative.duration && *alternative.duration > Rational::ZERO
                                             ? *alternative.duration
                                             : DEFAULT_FREEZE_DURATION,
                                     MIN_CLIP_DURATION);
                    // The clip's facts and its whole picture are read before the
                    // split mutates it, so the still clones the moving clip's
                    // every look field.
                    const Clip *found = project.active().clip(alternative.clip_id);
                    if (found == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    if (found->kind != ClipKind::Video && found->kind != ClipKind::Image) {
                        return Outcome{ std::nullopt, false };
                    }
                    const Rational offset = alternative.time - found->start;
                    if (offset <= MIN_CLIP_DURATION
                        || offset >= found->duration - MIN_CLIP_DURATION) {
                        return Outcome{ std::nullopt, false };
                    }
                    const ClipKind kind = found->kind;
                    const std::string media_id = found->media_id;
                    const std::string track_id = found->track_id;
                    const Rational clip_duration = found->duration;
                    const double speed = found->speed;
                    const Rational source_start = found->source_start;
                    const Clip picture = *found;

                    // The still's media: an image reuses its own; anything else
                    // needs the probed still, minted into the bin on first use.
                    std::string freeze_media_id;
                    if (kind == ClipKind::Image && !alternative.still) {
                        freeze_media_id = media_id;
                    } else {
                        if (!alternative.still) {
                            return Outcome{ std::nullopt, false };
                        }
                        const auto existing =
                                std::find_if(project.media.begin(), project.media.end(),
                                             [&alternative](const MediaItem &media) {
                                                 return media.path == alternative.still->path;
                                             });
                        if (existing != project.media.end()) {
                            freeze_media_id = existing->id;
                        } else {
                            freeze_media_id = mint.next("m");
                            MediaItem media;
                            media.id = freeze_media_id;
                            media.path = alternative.still->path;
                            media.name = alternative.still->name;
                            media.duration = alternative.still->duration;
                            media.kind = MediaKind::Image;
                            media.width = alternative.still->width;
                            media.height = alternative.still->height;
                            media.frame_rate = alternative.still->frame_rate;
                            media.frame_rate_fraction = alternative.still->frame_rate_fraction;
                            media.video_codec = alternative.still->video_codec;
                            media.origin = alternative.still->origin;
                            media.placeholder = false;
                            media.color_range = std::nullopt;
                            media.color_space = alternative.still->color_space;
                            project.media.push_back(std::move(media));
                            record_created(undo, freeze_media_id, std::in_place_type<MediaItem>);
                        }
                    }

                    Timeline &timeline = project.active_mut();
                    const auto it = std::find_if(timeline.clips.begin(), timeline.clips.end(),
                                                 [&alternative](const Clip &clip) {
                                                     return clip.id == alternative.clip_id;
                                                 });
                    if (it == timeline.clips.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    const std::size_t index = static_cast<std::size_t>(it - timeline.clips.begin());
                    record_before(undo, timeline.clips[index]);
                    {
                        // A curve does not survive a cut in halves: the map from
                        // here to the source is not affine, and the in-point below
                        // assumes it is, so both pieces go to the constant mean
                        // they averaged.
                        Clip &clip = timeline.clip_at_mut(index);
                        clip.speed_curve.reset();
                    }
                    const auto [head_source, tail_source] =
                            split_source(source_start, speed, offset);
                    const Rational whole = clip_duration;
                    Clip tail = timeline.clips[index];
                    tail.id = mint.next("c");
                    tail.start = alternative.time;
                    tail.duration = clip_duration - offset;
                    tail.source_start = tail_source;
                    tail.transition_in = std::nullopt;
                    tail.fade_in = Rational::ZERO;
                    tail.rewindow_keys(whole, offset, whole);
                    const std::string tail_id = tail.id;
                    record_created(undo, tail_id, std::in_place_type<Clip>);
                    Clip &head = timeline.clip_at_mut(index);
                    head.duration = offset;
                    head.source_start = head_source;
                    head.fade_out = Rational::ZERO;
                    head.rewindow_keys(whole, Rational::ZERO, offset);
                    timeline.clips.insert(timeline.clips.begin() + index + 1, std::move(tail));
                    // Ripple every later placement on this track (the new tail
                    // included) so the freeze does not sit on the remainder.
                    for (Clip *later : timeline.clips_where([&](const Clip &clip) {
                             return clip.track_id == track_id && clip.start >= alternative.time;
                         })) {
                        if (later->id != tail_id) {
                            record_before(undo, *later);
                        }
                        later->start = later->start + hold;
                    }
                    // The still is the source clip as a picture: cloned first so
                    // it carries every look, then the hold's facts overwrite the
                    // moving ones.
                    const std::string freeze_id = mint.next("c");
                    Clip frozen = picture;
                    frozen.id = freeze_id;
                    frozen.track_id = track_id;
                    frozen.kind = ClipKind::Image;
                    frozen.media_id = freeze_media_id;
                    frozen.start = alternative.time;
                    frozen.duration = hold;
                    frozen.source_start = Rational::ZERO;
                    frozen.speed = 1.0;
                    frozen.speed_curve.reset();
                    frozen.volume = 1.0;
                    frozen.fade_in = Rational::ZERO;
                    frozen.fade_out = Rational::ZERO;
                    frozen.filters.clear();
                    frozen.muted.reset();
                    frozen.detached_from.reset();
                    frozen.transition_in.reset();
                    frozen.text.reset();
                    record_created(undo, freeze_id, std::in_place_type<Clip>);
                    timeline.clips.push_back(std::move(frozen));
                    return Outcome{ freeze_id, true };
                }

                if constexpr (std::is_same_v<T, MergeClips>) {
                    // Rejoins split pieces into the earliest, which keeps its id
                    // and takes on the rest's span, keys and fade-out; refuses
                    // with why_not_merge's sentence when they cannot rejoin.
                    Timeline &timeline = project.active_mut();
                    if (const std::optional<std::string> reason =
                                why_not_merge(timeline, alternative.clip_ids)) {
                        return std::unexpected{ CommandError{ CommandErrorKind::CannotMerge,
                                                              *reason } };
                    }
                    std::vector<Clip> ordered;
                    ordered.reserve(alternative.clip_ids.size());
                    for (const std::string &id : alternative.clip_ids) {
                        const Clip *clip = timeline.clip(id);
                        if (clip != nullptr) {
                            ordered.push_back(*clip);
                        }
                    }
                    std::sort(ordered.begin(), ordered.end(),
                              [](const Clip &left, const Clip &right) {
                                  return left.start < right.start;
                              });
                    const Clip &first = ordered.front();
                    const Clip &last = ordered.back();
                    const Rational merged_duration = last.start + last.duration - first.start;
                    // Before-images: the survivor's and every absorbed piece's.
                    record_before(undo, first);
                    const std::string survivor_id = first.id;
                    std::vector<std::string> doomed;
                    doomed.reserve(ordered.size() - 1);
                    for (const Clip &piece : ordered) {
                        if (piece.id == survivor_id) {
                            continue;
                        }
                        doomed.push_back(piece.id);
                        record_before(undo, piece);
                    }
                    std::erase_if(timeline.clips, [&doomed](const Clip &clip) {
                        return std::find(doomed.begin(), doomed.end(), clip.id) != doomed.end();
                    });
                    Clip *survivor = timeline.clip_mut(survivor_id);
                    survivor->duration = merged_duration;
                    // Every piece's keys land where they were on the picture; the
                    // way out is the last piece's, as the way in is the first's.
                    survivor->rewindow_keys(first.duration, Rational::ZERO, merged_duration);
                    for (const Clip &piece : ordered) {
                        if (piece.id != survivor_id) {
                            survivor->absorb_keys(piece, piece.start - first.start);
                        }
                    }
                    survivor->fade_out = last.fade_out;
                    return Outcome{ survivor_id, true };
                }

                if constexpr (std::is_same_v<T, RemoveClips>) {
                    // Deletes named clips from the active timeline; unknown ids
                    // are ignored. With `ripple`, each track pulls every clip
                    // after a removed span left by the spans gone before it.
                    Timeline &timeline = project.active_mut();
                    const auto doomed = [&alternative](const std::string &id) {
                        return std::find(alternative.clip_ids.begin(), alternative.clip_ids.end(),
                                         id)
                                != alternative.clip_ids.end();
                    };
                    // The spans going, per track, taken before they are gone: the
                    // ripple closes exactly these.
                    std::vector<RemovedSpan> removed;
                    removed.reserve(alternative.clip_ids.size());
                    for (const Clip &clip : timeline.clips) {
                        if (doomed(clip.id)) {
                            removed.emplace_back(clip.track_id, clip.start,
                                                 clip.start + clip.duration);
                            record_before(undo, clip);
                        }
                    }
                    const std::size_t clip_count = timeline.clips.size();
                    std::erase_if(timeline.clips,
                                  [&doomed](const Clip &clip) { return doomed(clip.id); });
                    const bool applied = timeline.clips.size() != clip_count;
                    if (alternative.ripple && applied) {
                        close_gaps(timeline, removed, undo);
                    }
                    return Outcome{ std::nullopt, applied };
                }

                // `apply_clips` routes only reshaping commands here.
                std::unreachable();
            },
            command.value);
}

} // namespace genesis::project
