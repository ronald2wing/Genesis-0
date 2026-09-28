// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "project/commands/Apply.h"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace genesis::project {

// Lanes: adding, removing and muting or hiding them.
//
// One arm per command, exactly as `apply` routes them here; everything these
// arms share lives in the parent module.
ApplyResult apply_tracks(const Command &command, Project &project, IdMinter &mint, UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, AddTrack>) {
                    // Appends a lane named after the highest "Track N" in use,
                    // minting a "t" id.
                    Timeline &timeline = project.active_mut();
                    const std::string id = mint.next("t");
                    Track track;
                    track.id = id;
                    track.visible = true;
                    track.muted = false;
                    timeline.tracks.push_back(std::move(track));
                    record_created(undo, id, std::in_place_type<Track>);
                    return Outcome{ id, true };
                }

                if constexpr (std::is_same_v<T, RemoveTrack>) {
                    // Deletes a lane and every clip on it. Errs at the floor of
                    // one track; an unknown id removes nothing and is a no-op.
                    Timeline &timeline = project.active_mut();
                    if (timeline.tracks.size() <= 1) {
                        return std::unexpected{ CommandError{ CommandErrorKind::LastTrack, { } } };
                    }
                    const auto track_it =
                            std::find_if(timeline.tracks.begin(), timeline.tracks.end(),
                                         [&alternative](const Track &track) {
                                             return track.id == alternative.track_id;
                                         });
                    if (track_it == timeline.tracks.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *track_it);
                    for (const Clip &clip : timeline.clips) {
                        if (clip.track_id == alternative.track_id) {
                            record_before(undo, clip);
                        }
                    }
                    std::erase_if(timeline.tracks, [&alternative](const Track &track) {
                        return track.id == alternative.track_id;
                    });
                    std::erase_if(timeline.clips, [&alternative](const Clip &clip) {
                        return clip.track_id == alternative.track_id;
                    });
                    return Outcome{ std::nullopt, true };
                }

                if constexpr (std::is_same_v<T, SetTrackFlag>) {
                    // Flips one of a track's two toggles. An unknown id is a
                    // no-op; setting the value it already holds reports false.
                    Timeline &timeline = project.active_mut();
                    const auto track_it =
                            std::find_if(timeline.tracks.begin(), timeline.tracks.end(),
                                         [&alternative](const Track &track) {
                                             return track.id == alternative.track_id;
                                         });
                    if (track_it == timeline.tracks.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *track_it);
                    const bool changed = alternative.flag == TrackFlag::Visible
                            ? assign(track_it->visible, alternative.value)
                            : assign(track_it->muted, alternative.value);
                    return Outcome{ std::nullopt, changed };
                }

                // `apply` routes only this module's commands here.
                std::unreachable();
            },
            command.value);
}

} // namespace genesis::project
