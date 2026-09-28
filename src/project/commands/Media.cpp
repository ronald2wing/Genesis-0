// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "project/commands/Apply.h"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace genesis::project {

// The bin and the fonts: what a project can place, before anything is placed.
//
// One arm per command, exactly as `apply` routes them here; everything these
// arms share lives in the parent module.
ApplyResult apply_media(const Command &command, Project &project, IdMinter &mint, UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, AddMedia>) {
                    // A path already in the bin is a tolerated no-op, so re-imports
                    // cannot duplicate media.
                    const bool duplicate =
                            std::any_of(project.media.begin(), project.media.end(),
                                        [&alternative](const MediaItem &existing) {
                                            return existing.path == alternative.item.path;
                                        });
                    if (duplicate) {
                        return Outcome{ std::nullopt, false };
                    }
                    const std::string id = mint.next("m");
                    MediaItem media;
                    media.id = id;
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
                    record_created(undo, id, std::in_place_type<MediaItem>);
                    return Outcome{ id, true };
                }

                if constexpr (std::is_same_v<T, SetMediaPlaceholder>) {
                    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                                 [&alternative](const MediaItem &item) {
                                                     return item.id == alternative.media_id;
                                                 });
                    if (it == project.media.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    const bool applied = assign(it->placeholder, alternative.placeholder);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, FillSlot>) {
                    const auto media_it =
                            std::find_if(project.media.begin(), project.media.end(),
                                         [&alternative](const MediaItem &existing) {
                                             return existing.id == alternative.media_id;
                                         });
                    if (media_it == project.media.end()) {
                        return std::unexpected{ CommandError{ CommandErrorKind::SlotGone, { } } };
                    }
                    if (!media_it->placeholder) {
                        return std::unexpected{ CommandError{ CommandErrorKind::NotASlot, { } } };
                    }

                    // The slot keeps its id, so every clip that references it keeps
                    // working; only the identity behind the id changes.
                    record_before(undo, *media_it);
                    const NewMedia &item = alternative.item;
                    media_it->path = item.path;
                    media_it->name = item.name;
                    media_it->duration = item.duration;
                    media_it->kind = item.kind;
                    media_it->width = item.width;
                    media_it->height = item.height;
                    media_it->frame_rate = item.frame_rate;
                    media_it->frame_rate_fraction = item.frame_rate_fraction;
                    media_it->video_codec = item.video_codec;
                    media_it->audio_codec = item.audio_codec;
                    media_it->has_audio = item.has_audio;
                    media_it->audio_tracks = item.audio_tracks;
                    media_it->color_space = item.color_space;
                    media_it->placeholder = false;

                    ClipKind kind = ClipKind::Video;
                    switch (item.kind) {
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

                    // Slot timing is the template's: start, duration and speed stay
                    // put, which is what keeps cuts on the beat. The in-point resets
                    // because it referred to the old footage; a clip shorter than its
                    // slot freeze-frames on its last frame downstream, which is the
                    // renderer's existing behaviour for a trim past the media's end.
                    // All timelines, like RemoveMedia: slots are not per-timeline.
                    for (Timeline &timeline : project.timelines) {
                        // A timeline with no clip of this media stays the
                        // snapshot's.
                        const bool referenced =
                                std::any_of(timeline.clips.begin(), timeline.clips.end(),
                                            [&alternative](const Clip &clip) {
                                                return clip.media_id == alternative.media_id;
                                            });
                        if (!referenced) {
                            continue;
                        }
                        for (Clip &clip : timeline.clips) {
                            if (clip.media_id != alternative.media_id) {
                                continue;
                            }
                            record_before(undo, clip);
                            clip.source_start = Rational::ZERO;
                            clip.kind = kind;
                            clip.name = item.name;
                            // A stream index named against the old file means
                            // nothing against the new one.
                            clip.audio_stream = std::nullopt;
                        }
                    }
                    // Filling always changes the project: the target was a
                    // placeholder and is one no longer.
                    return Outcome{ std::nullopt, true };
                }

                if constexpr (std::is_same_v<T, RemoveMedia>) {
                    // All timelines, not just the active one: a shelved clip whose
                    // media is gone would linger as a dead reference.
                    const auto media_it = std::find_if(project.media.begin(), project.media.end(),
                                                       [&alternative](const MediaItem &item) {
                                                           return item.id == alternative.media_id;
                                                       });
                    bool applied = false;
                    if (media_it != project.media.end()) {
                        record_before(undo, *media_it);
                        project.media.erase(media_it);
                        applied = true;
                    }
                    for (Timeline &timeline : project.timelines) {
                        for (const Clip &clip : timeline.clips) {
                            if (clip.media_id == alternative.media_id) {
                                record_before(undo, clip);
                            }
                        }
                        const std::size_t clip_count = timeline.clips.size();
                        std::erase_if(timeline.clips, [&alternative](const Clip &clip) {
                            return clip.media_id == alternative.media_id;
                        });
                        applied = applied || timeline.clips.size() != clip_count;
                    }
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, AddFont>) {
                    // A path already registered is a no-op, so re-adding cannot
                    // duplicate.
                    const bool duplicate = std::any_of(project.fonts.begin(), project.fonts.end(),
                                                       [&alternative](const CustomFont &font) {
                                                           return font.path == alternative.path;
                                                       });
                    if (duplicate) {
                        return Outcome{ std::nullopt, false };
                    }
                    CustomFont font;
                    font.family = alternative.family;
                    font.path = alternative.path;
                    project.fonts.push_back(std::move(font));
                    record_created(undo, alternative.family, std::in_place_type<CustomFont>);
                    return Outcome{ std::nullopt, true };
                }

                if constexpr (std::is_same_v<T, RemoveFont>) {
                    // Clips keep the family name: the face may come back when the
                    // file does.
                    const auto it = std::find_if(project.fonts.begin(), project.fonts.end(),
                                                 [&alternative](const CustomFont &font) {
                                                     return font.family == alternative.family;
                                                 });
                    if (it == project.fonts.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    project.fonts.erase(it);
                    return Outcome{ std::nullopt, true };
                }

                if constexpr (std::is_same_v<T, UpdateMediaPath>) {
                    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                                 [&alternative](const MediaItem &item) {
                                                     return item.id == alternative.media_id;
                                                 });
                    if (it == project.media.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    const bool applied = assign(it->path, alternative.new_path);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SetMediaColorRange>) {
                    const auto it = std::find_if(project.media.begin(), project.media.end(),
                                                 [&alternative](const MediaItem &item) {
                                                     return item.id == alternative.media_id;
                                                 });
                    if (it == project.media.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    const bool applied = assign(it->color_range, alternative.range);
                    return Outcome{ std::nullopt, applied };
                }

                // `apply` routes only this module's commands here.
                std::unreachable();
            },
            command.value);
}

} // namespace genesis::project
