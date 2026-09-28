// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-project/src/commands/timelines.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "project/commands/Apply.h"
#include "project/Naming.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace genesis::project {

// Timelines: the tabs a project holds, their frames, names, order and which
// one is showing.
//
// One arm per command, exactly as `apply` routes them here; everything these
// arms share lives in the parent module.
ApplyResult apply_timelines(const Command &command, Project &project, IdMinter &mint,
                            UndoRecord &undo)
{
    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, AddTimeline>) {
                    // Adds a fresh timeline - four new lanes, "Timeline N" after
                    // the highest in use - and makes it active. Mints a "tl" id.
                    const std::string id = mint.next("tl");
                    const std::string name = next_numbered("Timeline", project.timelines);
                    std::vector<Track> tracks;
                    tracks.reserve(4);
                    for (int lane = 0; lane < 4; ++lane) {
                        Track track;
                        track.id = mint.next("t");
                        track.visible = true;
                        track.muted = false;
                        tracks.push_back(std::move(track));
                    }
                    // Born at the frame of the timeline you were looking at: the
                    // likeliest second timeline is another cut of the same picture,
                    // and the one that is not is a sheet away from being told so.
                    const VideoSettings video = project.active().video;
                    Timeline timeline;
                    timeline.id = id;
                    timeline.name = name;
                    timeline.video = video;
                    timeline.tracks = std::move(tracks);
                    project.timelines.push_back(std::move(timeline));
                    project.active_timeline_id = id;
                    record_created(undo, id, std::in_place_type<Timeline>);
                    return Outcome{ id, true };
                }

                if constexpr (std::is_same_v<T, SetTimelineVideo>) {
                    // Sets a timeline's output frame and rate. A term no frame
                    // could have, such as a zero dimension or a zero rate, is
                    // refused as a no-op rather than clamped.
                    if (!alternative.video.is_sane()) {
                        return Outcome{ std::nullopt, false };
                    }
                    const auto it = std::find_if(project.timelines.begin(), project.timelines.end(),
                                                 [&alternative](const Timeline &timeline) {
                                                     return timeline.id == alternative.timeline_id;
                                                 });
                    if (it == project.timelines.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    const bool applied = assign(it->video, alternative.video);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, RemoveTimeline>) {
                    // Deletes a timeline, moving the active tab to a neighbour if
                    // it was this one. Errs at the floor of one timeline.
                    if (project.timelines.size() <= 1) {
                        return std::unexpected{ CommandError{ CommandErrorKind::LastTimeline,
                                                              { } } };
                    }
                    const auto it = std::find_if(project.timelines.begin(), project.timelines.end(),
                                                 [&alternative](const Timeline &timeline) {
                                                     return timeline.id == alternative.timeline_id;
                                                 });
                    if (it == project.timelines.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    if (project.active_timeline_id == alternative.timeline_id) {
                        const std::size_t index =
                                static_cast<std::size_t>(it - project.timelines.begin());
                        const std::size_t neighbour =
                                index + 1 < project.timelines.size() ? index + 1 : index - 1;
                        project.active_timeline_id = project.timelines[neighbour].id;
                    }
                    project.timelines.erase(it);
                    return Outcome{ std::nullopt, true };
                }

                if constexpr (std::is_same_v<T, RenameTimeline>) {
                    // Renames a timeline tab. Whitespace-only names are ignored so
                    // a tab can never end up blank; unknown ids are tolerated.
                    const std::string trimmed = trim(alternative.name);
                    if (trimmed.empty()) {
                        return Outcome{ std::nullopt, false };
                    }
                    const auto it = std::find_if(project.timelines.begin(), project.timelines.end(),
                                                 [&alternative](const Timeline &timeline) {
                                                     return timeline.id == alternative.timeline_id;
                                                 });
                    if (it == project.timelines.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    record_before(undo, *it);
                    const bool applied = assign(it->name, trimmed);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, SelectTimeline>) {
                    // Switches which timeline subsequent commands act on. An
                    // unknown id leaves the selection where it was.
                    //
                    // Short-circuiting is right here: an unknown id must not touch
                    // the selection at all.
                    const bool applied =
                            std::any_of(project.timelines.begin(), project.timelines.end(),
                                        [&alternative](const Timeline &timeline) {
                                            return timeline.id == alternative.timeline_id;
                                        })
                            && assign(project.active_timeline_id, alternative.timeline_id);
                    return Outcome{ std::nullopt, applied };
                }

                if constexpr (std::is_same_v<T, MoveTimeline>) {
                    // Moves a timeline tab to a new position in the strip. Which
                    // timeline is active does not change - only the order of the
                    // tabs.
                    const auto it = std::find_if(project.timelines.begin(), project.timelines.end(),
                                                 [&alternative](const Timeline &timeline) {
                                                     return timeline.id == alternative.timeline_id;
                                                 });
                    if (it == project.timelines.end()) {
                        return Outcome{ std::nullopt, false };
                    }
                    const std::size_t from =
                            static_cast<std::size_t>(it - project.timelines.begin());
                    const std::size_t to =
                            std::min(alternative.index, project.timelines.size() - 1);
                    if (to == from) {
                        return Outcome{ std::nullopt, false };
                    }
                    // A reorder is not a property of any one timeline: the undo
                    // layer rebuilds the tab order from the snapshots' own order,
                    // so snapshot every timeline, in order.
                    for (const Timeline &timeline : project.timelines) {
                        record_before(undo, timeline);
                    }
                    Timeline timeline = std::move(*it);
                    project.timelines.erase(it);
                    project.timelines.insert(project.timelines.begin()
                                                     + static_cast<std::ptrdiff_t>(to),
                                             std::move(timeline));
                    return Outcome{ std::nullopt, true };
                }

                // `apply` routes only this module's commands here.
                std::unreachable();
            },
            command.value);
}

} // namespace genesis::project
