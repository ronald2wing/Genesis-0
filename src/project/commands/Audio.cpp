// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "project/commands/Apply.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace genesis::project {

// Taking a video's sound out onto its own lane, and putting it back.
//
// One arm per command, exactly as `apply` routes them here; everything these
// arms share lives in the parent module.
ApplyResult apply_audio(const Command &command, Project &project, IdMinter &mint, UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, DetachAudio>) {
                    // What comes off, as `(stream, name)`: one sound clip per
                    // audio track when the file lists several - a recording that
                    // kept the desktop and the microphone apart stays apart, each
                    // on a lane of its own - else the one stream the video clip
                    // was playing.
                    using Sound = std::pair<std::optional<std::uint32_t>, std::string>;
                    std::vector<Sound> sounds;
                    Clip source;
                    {
                        const Timeline &timeline = project.active();
                        const Clip *clip = timeline.clip(alternative.clip_id);
                        if (clip == nullptr) {
                            return Outcome{ std::nullopt, false };
                        }
                        source = *clip;
                        const MediaItem *media = project.media_by_id(clip->media_id);
                        const bool has_audio = clip->kind == ClipKind::Video
                                && clip->muted != std::optional<bool>{ true } && media != nullptr
                                && media->has_audio
                                && std::none_of(timeline.clips.begin(), timeline.clips.end(),
                                                [&alternative](const Clip &other) {
                                                    return other.detached_from
                                                            && *other.detached_from
                                                            == alternative.clip_id;
                                                });
                        if (!has_audio) {
                            return Outcome{ std::nullopt, false };
                        }
                        if (media != nullptr && media->audio_tracks.size() > 1) {
                            for (std::size_t position = 0; position < media->audio_tracks.size();
                                 ++position) {
                                const AudioTrack &track = media->audio_tracks[position];
                                const std::string label = track.title.empty()
                                        ? "Track " + std::to_string(position + 1)
                                        : track.title;
                                sounds.emplace_back(track.index, clip->name + " · " + label);
                            }
                        } else {
                            sounds.emplace_back(clip->audio_stream, clip->name);
                        }
                    }

                    Timeline &timeline = project.active_mut();
                    std::optional<std::string> first_sound;
                    for (const auto &[stream, name] : sounds) {
                        // A lane free for the whole span, or a fresh one. Each
                        // sound placed takes its lane, so the next looks past it.
                        const Rational end = source.start + source.duration;
                        const auto free_track = std::find_if(
                                timeline.tracks.begin(), timeline.tracks.end(),
                                [&](const Track &track) {
                                    return std::none_of(timeline.clips.begin(),
                                                        timeline.clips.end(),
                                                        [&](const Clip &other) {
                                                            return other.track_id == track.id
                                                                    && other.start < end
                                                                    && source.start
                                                                    < other.start + other.duration;
                                                        });
                                });
                        std::string track_id;
                        if (free_track != timeline.tracks.end()) {
                            track_id = free_track->id;
                        } else {
                            track_id = mint.next("t");
                            Track track;
                            track.id = track_id;
                            track.visible = true;
                            track.muted = false;
                            timeline.tracks.push_back(std::move(track));
                            record_created(undo, track_id, std::in_place_type<Track>);
                        }

                        Clip sound = source;
                        sound.id = mint.next("c");
                        sound.track_id = track_id;
                        sound.name = name;
                        sound.kind = ClipKind::Audio;
                        sound.video_effects.clear();
                        sound.transition_in.reset();
                        sound.detached_from = alternative.clip_id;
                        sound.muted.reset();
                        sound.audio_stream = stream;
                        if (!first_sound) {
                            first_sound = sound.id;
                        }
                        record_created(undo, sound.id, std::in_place_type<Clip>);
                        timeline.clips.push_back(std::move(sound));
                    }

                    Clip *video = timeline.clip_mut(alternative.clip_id);
                    record_before(undo, *video);
                    video->muted = true;
                    video->filters.clear();
                    return Outcome{ first_sound, true };
                }

                if constexpr (std::is_same_v<T, ReattachAudio>) {
                    Timeline &timeline = project.active_mut();
                    const Clip *clip = timeline.clip(alternative.clip_id);
                    if (clip == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    const std::string video_id =
                            clip->kind == ClipKind::Audio && clip->detached_from
                            ? *clip->detached_from
                            : clip->id;
                    if (timeline.clip(video_id) == nullptr) {
                        return Outcome{ std::nullopt, false };
                    }
                    std::vector<Clip> sounds;
                    for (const Clip &other : timeline.clips) {
                        if (other.detached_from && *other.detached_from == video_id) {
                            sounds.push_back(other);
                        }
                    }
                    if (sounds.empty()) {
                        return Outcome{ std::nullopt, false };
                    }
                    // The deletions are recorded as before-images, so undo puts
                    // the sound clips back. A set, not a vector: the erase below
                    // tests every clip on the timeline against it.
                    for (const Clip &sound : sounds) {
                        record_before(undo, sound);
                    }
                    std::erase_if(timeline.clips, [&sounds](const Clip &other) {
                        return std::any_of(
                                sounds.begin(), sounds.end(),
                                [&other](const Clip &sound) { return sound.id == other.id; });
                    });
                    Clip *video = timeline.clip_mut(video_id);
                    record_before(undo, *video);
                    video->muted.reset();
                    video->filters = sounds.front().filters;
                    // Reaching here means at least one sound clip was deleted.
                    return Outcome{ std::nullopt, true };
                }

                // `apply` routes only this module's commands here.
                std::unreachable();
            },
            command.value);
}

} // namespace genesis::project
