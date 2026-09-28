// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-project/src/commands/clips.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "project/commands/Apply.h"
#include "project/Naming.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace genesis::project {
namespace {

// Fallback length for media whose container reports no duration.
const Rational UNKNOWN_DURATION = Rational{ 5, 1 };
// How long a still lasts when first placed. Editorial default, not a fact.
const Rational DEFAULT_IMAGE_DURATION = Rational{ 5, 1 };
// How long a title lasts when first placed. Editorial default, not a fact.
const Rational DEFAULT_TEXT_DURATION = Rational{ 4, 1 };
// How long a layer covers when first placed. Editorial default, not a fact.
const Rational DEFAULT_LAYER_DURATION = Rational{ 5, 1 };
// The floor on a clip's timeline length: a sixtieth of a second.
const Rational MIN_CLIP_DURATION = Rational{ 1, 60 };
// How far a title may sit from the centre, as a frame-height fraction.
constexpr double MAX_OFFSET = 3.0;

Clip default_clip(std::string id, std::string track_id, const MediaItem &media, Rational start)
{
    ClipKind kind = ClipKind::Video;
    switch (media.kind) {
    case MediaKind::Video:
        kind = ClipKind::Video;
        break;
    case MediaKind::Audio:
        kind = ClipKind::Audio;
        break;
    case MediaKind::Image:
        kind = ClipKind::Image;
        break;
    }
    const Rational duration = media.kind == MediaKind::Image
            ? DEFAULT_IMAGE_DURATION
            : media.duration.value_or(UNKNOWN_DURATION);
    Clip clip = Clip::blank(std::move(id), std::move(track_id), kind, media.name, start, duration);
    clip.media_id = media.id;
    return clip;
}

// Makes room on `track_id` for a new clip at `start`: a drop onto the
// middle of a clip lands at that clip's end instead, and every clip at
// or after the place it lands moves right by the new clip's length, so
// the new clip slots in and nothing is covered (#129). Returns where the
// new clip lands. A drop with room to spare changes nothing.
Rational ripple_room_for(Timeline &timeline, std::string_view track_id, Rational start,
                         const MediaItem &media, UndoRecord &undo)
{
    const Rational duration = media.kind == MediaKind::Image
            ? DEFAULT_IMAGE_DURATION
            : media.duration.value_or(UNKNOWN_DURATION);
    // Dropped onto a clip: after it, rather than over it or through it.
    Rational landing = start;
    for (const Clip &clip : timeline.clips) {
        if (clip.track_id == track_id && clip.start < start && start < clip.start + clip.duration) {
            landing = std::max(landing, clip.start + clip.duration);
        }
    }
    const Rational end = landing + duration;
    const bool overlaps =
            std::any_of(timeline.clips.begin(), timeline.clips.end(), [&](const Clip &clip) {
                return clip.track_id == track_id && clip.start < end
                        && landing < clip.start + clip.duration;
            });
    if (!overlaps) {
        return landing;
    }
    for (Clip *clip : timeline.clips_where([&](const Clip &clip) {
             return clip.track_id == track_id && clip.start >= landing;
         })) {
        record_before(undo, *clip);
        clip->start = clip->start + duration;
    }
    return landing;
}

// The lowest track with nothing occupying `[start, start + duration)`,
// falling back to the bottom track.
std::optional<std::string> first_free_track(const Timeline &timeline, Rational start,
                                            Rational duration)
{
    const Rational end = start + duration;
    const auto it =
            std::find_if(timeline.tracks.begin(), timeline.tracks.end(), [&](const Track &track) {
                return std::none_of(timeline.clips.begin(), timeline.clips.end(),
                                    [&](const Clip &clip) {
                                        return clip.track_id == track.id && clip.start < end
                                                && start < clip.start + clip.duration;
                                    });
            });
    if (it != timeline.tracks.end()) {
        return it->id;
    }
    if (timeline.tracks.empty()) {
        return std::nullopt;
    }
    return timeline.tracks.front().id;
}

// First free lane *above* the highest one occupied over `[start, start +
// duration)`. `None` when every lane above is taken, which is the caller's
// cue to mint a new one at the top.
//
// Captions and titles go through this so they sit over the video, not
// under it. The plain `first_free_track` still walks from the bottom,
// which is what a sound or a picture wants.
std::optional<std::string> first_free_track_above(const Timeline &timeline, Rational start,
                                                  Rational duration)
{
    const Rational end = start + duration;
    const auto occupied = [&](std::string_view track_id) {
        return std::any_of(timeline.clips.begin(), timeline.clips.end(), [&](const Clip &clip) {
            return clip.track_id == track_id && clip.start < end
                    && start < clip.start + clip.duration;
        });
    };
    // The lane above the highest occupied one, or the bottom lane when
    // none is occupied over the span.
    std::size_t floor = 0;
    for (std::size_t row = 0; row < timeline.tracks.size(); ++row) {
        if (occupied(timeline.tracks[row].id)) {
            floor = row + 1;
        }
    }
    for (std::size_t row = floor; row < timeline.tracks.size(); ++row) {
        if (!occupied(timeline.tracks[row].id)) {
            return timeline.tracks[row].id;
        }
    }
    return std::nullopt;
}

} // namespace

// The reshaping half lives in ClipReshape.cpp; `apply_clips` forwards the
// clip-reshaping commands there and places the rest here.
ApplyResult apply_clip_reshape(const Command &, Project &, IdMinter &, UndoRecord &);

// Placing and moving clips: the edits that put something on the timeline and
// change when it starts.
//
// One arm per command, exactly as `apply_clips` routes them here; everything
// these arms share lives in the parent module.
ApplyResult apply_clip_placement(const Command &command, Project &project, IdMinter &mint,
                                 UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, AddClip>) {
                    // Places a clip on a named track, minting a "c" id. Duration
                    // comes from the media (five seconds for a still or unknown
                    // length); errs if the media or track no longer exists.
                    const MediaItem *found = project.media_by_id(alternative.media_id);
                    if (found == nullptr) {
                        return std::unexpected{ CommandError{ CommandErrorKind::MediaGone, { } } };
                    }
                    // Copied out of the bin, as the Rust clones, so the placement
                    // below never holds a bin reference across the timeline edit.
                    const MediaItem media = *found;
                    Timeline &timeline = project.active_mut();
                    if (timeline.track(alternative.track_id) == nullptr) {
                        return std::unexpected{ CommandError{ CommandErrorKind::TrackGone, { } } };
                    }
                    Rational start = alternative.start;
                    if (alternative.ripple) {
                        start = ripple_room_for(timeline, alternative.track_id, start, media, undo);
                    }
                    const std::string id = mint.next("c");
                    Clip clip = default_clip(id, alternative.track_id, media, start);
                    record_created(undo, id, std::in_place_type<Clip>);
                    timeline.clips.push_back(std::move(clip));
                    return Outcome{ id, true };
                }

                if constexpr (std::is_same_v<T, AddClipAtFirstFree>) {
                    // AddClip without naming a lane: lands on the lowest track
                    // with nothing in the clip's span, falling back to the bottom
                    // track (overlap and all) rather than refusing.
                    const MediaItem *found = project.media_by_id(alternative.media_id);
                    if (found == nullptr) {
                        return std::unexpected{ CommandError{ CommandErrorKind::MediaGone, { } } };
                    }
                    const MediaItem media = *found;
                    const Rational duration = media.kind == MediaKind::Image
                            ? DEFAULT_IMAGE_DURATION
                            : media.duration.value_or(UNKNOWN_DURATION);
                    Timeline &timeline = project.active_mut();
                    const std::optional<std::string> track_id =
                            first_free_track(timeline, alternative.start, duration);
                    if (!track_id) {
                        return std::unexpected{ CommandError{ CommandErrorKind::NoTracks, { } } };
                    }
                    const std::string id = mint.next("c");
                    Clip clip = default_clip(id, *track_id, media, alternative.start);
                    record_created(undo, id, std::in_place_type<Clip>);
                    timeline.clips.push_back(std::move(clip));
                    return Outcome{ id, true };
                }

                if constexpr (std::is_same_v<T, AddTextClip>) {
                    // Places a title: a clip with no media behind it, named after
                    // the text's first line, minting a "c" id. A named lane must
                    // still exist; an absent one with `above` set lands on the
                    // first free lane above the highest one occupied over the
                    // span, minting a new lane at the top when every one above is
                    // taken, so words sit over the video rather than under it.
                    const TextStyle style = alternative.style.value_or(TextStyle{ });
                    const Rational duration =
                            std::max(alternative.duration.value_or(DEFAULT_TEXT_DURATION),
                                     MIN_CLIP_DURATION);
                    Timeline &timeline = project.active_mut();
                    std::string track_id;
                    if (alternative.track_id) {
                        if (timeline.track(*alternative.track_id) == nullptr) {
                            return std::unexpected{ CommandError{ CommandErrorKind::TrackGone,
                                                                  { } } };
                        }
                        track_id = *alternative.track_id;
                    } else if (alternative.above) {
                        const std::optional<std::string> lane =
                                first_free_track_above(timeline, alternative.start, duration);
                        if (lane) {
                            track_id = *lane;
                        } else {
                            // Every lane above the video is taken: mint one at
                            // the top for the words to land on.
                            track_id = mint.next("t");
                            Track track;
                            track.id = track_id;
                            track.visible = true;
                            track.muted = false;
                            timeline.tracks.push_back(std::move(track));
                            record_created(undo, track_id, std::in_place_type<Track>);
                        }
                    } else {
                        const std::optional<std::string> lane =
                                first_free_track(timeline, alternative.start, duration);
                        if (!lane) {
                            return std::unexpected{ CommandError{ CommandErrorKind::NoTracks,
                                                                  { } } };
                        }
                        track_id = *lane;
                    }
                    const std::string id = mint.next("c");
                    Clip clip = Clip::blank(id, track_id, ClipKind::Text, first_line(style.content),
                                            alternative.start, duration);
                    clip.offset_y =
                            std::clamp(alternative.offset_y.value_or(0.0), -MAX_OFFSET, MAX_OFFSET);
                    clip.text = style;
                    record_created(undo, id, std::in_place_type<Clip>);
                    timeline.clips.push_back(std::move(clip));
                    return Outcome{ id, true };
                }

                if constexpr (std::is_same_v<T, AddLayerClip>) {
                    // Places a layer: a look or an effect over a span of the
                    // timeline that treats everything beneath it, with no media
                    // behind it. Named after the effect unless a name was given;
                    // the chain starts as the one package at its defaults.
                    const Rational duration =
                            std::max(alternative.duration.value_or(DEFAULT_LAYER_DURATION),
                                     MIN_CLIP_DURATION);
                    Timeline &timeline = project.active_mut();
                    std::string track_id;
                    if (alternative.track_id) {
                        if (timeline.track(*alternative.track_id) == nullptr) {
                            return std::unexpected{ CommandError{ CommandErrorKind::TrackGone,
                                                                  { } } };
                        }
                        track_id = *alternative.track_id;
                    } else {
                        const std::optional<std::string> lane =
                                first_free_track(timeline, alternative.start, duration);
                        if (!lane) {
                            return std::unexpected{ CommandError{ CommandErrorKind::NoTracks,
                                                                  { } } };
                        }
                        track_id = *lane;
                    }
                    const std::string id = mint.next("c");
                    const std::string name = trim(alternative.name).empty() ? alternative.effect_id
                                                                            : alternative.name;
                    Clip clip = Clip::blank(id, track_id, ClipKind::Layer, name, alternative.start,
                                            duration);
                    clip.video_effects.push_back(AppliedFilter::create(alternative.effect_id));
                    record_created(undo, id, std::in_place_type<Clip>);
                    timeline.clips.push_back(std::move(clip));
                    return Outcome{ id, true };
                }

                if constexpr (std::is_same_v<T, MoveClips>) {
                    // Repositions any number of clips in one edit - one undo step
                    // for a whole multi-selection drag. An unknown clip is
                    // skipped; a start is floored at zero; a track id this
                    // timeline does not have moves the clip in time but leaves it
                    // on its current track.
                    Timeline &timeline = project.active_mut();
                    bool applied = false;
                    for (const ClipMove &wanted : alternative.moves) {
                        Clip *clip = timeline.clip_mut(wanted.clip_id);
                        if (clip == nullptr) {
                            continue;
                        }
                        record_before(undo, *clip);
                        applied |= assign(clip->start, std::max(wanted.start, Rational::ZERO));
                        if (timeline.track(wanted.track_id) != nullptr) {
                            applied |= assign(clip->track_id, wanted.track_id);
                        }
                    }
                    return Outcome{ std::nullopt, applied };
                }

                // `apply_clips` routes only placement commands here.
                std::unreachable();
            },
            command.value);
}

// The dispatcher: routes each clip command to its half - placement here,
// reshaping in ClipReshape.cpp.
ApplyResult apply_clips(const Command &command, Project &project, IdMinter &mint, UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, AddClip> || std::is_same_v<T, AddClipAtFirstFree>
                              || std::is_same_v<T, AddTextClip> || std::is_same_v<T, AddLayerClip>
                              || std::is_same_v<T, MoveClips>) {
                    return apply_clip_placement(command, project, mint, undo);
                }

                return apply_clip_reshape(command, project, mint, undo);
            },
            command.value);
}

} // namespace genesis::project
