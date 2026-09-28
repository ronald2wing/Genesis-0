// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "project/commands/Apply.h"

#include <iterator>
#include <type_traits>
#include <utility>
#include <variant>

#include "project/command/Validate.h"

namespace genesis::project {

// The dispatcher: guards non-finite values, routes every other command to
// its verb group, and makes a Batch atomic without cloning the project.
ApplyResult apply(const Command &command, Project &project, IdMinter &mint, UndoRecord &undo)
{
    // One NaN or infinity would poison every duration and key it touched;
    // refuse before any dispatch.
    if (has_non_finite(command)) {
        return std::unexpected{ CommandError{ CommandErrorKind::NotANumber, { } } };
    }

    return std::visit(
            [&](const auto &alternative) -> ApplyResult {
                using T = std::decay_t<decltype(alternative)>;

                if constexpr (std::is_same_v<T, Batch>) {
                    // All or nothing, without cloning the project: apply each
                    // member into a pending record and revert on the first
                    // failure, so one bad command cannot leave a half-applied
                    // batch behind (and the editor records it as one undo step).
                    UndoRecord pending;
                    pending.id_counter = mint.counter();
                    pending.active_timeline_id = project.active_timeline_id;

                    std::optional<std::string> created_id;
                    bool applied = false;
                    for (const Command &member : alternative.commands) {
                        ApplyResult outcome = apply(member, project, mint, pending);
                        if (!outcome) {
                            restore_before(project, mint, pending);
                            return std::unexpected{ std::move(outcome.error()) };
                        }
                        if (outcome->created_id) {
                            created_id = outcome->created_id;
                        }
                        applied |= outcome->applied;
                    }

                    // The batch's before-images join the caller's record, so the
                    // whole batch reverses as one undo step.
                    undo.clips.insert(undo.clips.end(),
                                      std::make_move_iterator(pending.clips.begin()),
                                      std::make_move_iterator(pending.clips.end()));
                    undo.tracks.insert(undo.tracks.end(),
                                       std::make_move_iterator(pending.tracks.begin()),
                                       std::make_move_iterator(pending.tracks.end()));
                    undo.timelines.insert(undo.timelines.end(),
                                          std::make_move_iterator(pending.timelines.begin()),
                                          std::make_move_iterator(pending.timelines.end()));
                    undo.media.insert(undo.media.end(),
                                      std::make_move_iterator(pending.media.begin()),
                                      std::make_move_iterator(pending.media.end()));
                    undo.fonts.insert(undo.fonts.end(),
                                      std::make_move_iterator(pending.fonts.begin()),
                                      std::make_move_iterator(pending.fonts.end()));
                    return Outcome{ created_id, applied };
                }

                if constexpr (std::is_same_v<T, AddMedia> || std::is_same_v<T, SetMediaPlaceholder>
                              || std::is_same_v<T, FillSlot> || std::is_same_v<T, RemoveMedia>
                              || std::is_same_v<T, AddFont> || std::is_same_v<T, RemoveFont>
                              || std::is_same_v<T, UpdateMediaPath>
                              || std::is_same_v<T, SetMediaColorRange>) {
                    return apply_media(command, project, mint, undo);
                }

                if constexpr (std::is_same_v<T, AddClip> || std::is_same_v<T, AddClipAtFirstFree>
                              || std::is_same_v<T, AddTextClip> || std::is_same_v<T, AddLayerClip>
                              || std::is_same_v<T, MoveClips> || std::is_same_v<T, TrimClip>
                              || std::is_same_v<T, SplitClips> || std::is_same_v<T, FreezeFrame>
                              || std::is_same_v<T, ReplaceClipMedia>
                              || std::is_same_v<T, MergeClips> || std::is_same_v<T, RemoveClips>) {
                    return apply_clips(command, project, mint, undo);
                }

                if constexpr (std::is_same_v<T, UpdateClip> || std::is_same_v<T, SetClipSpeed>
                              || std::is_same_v<T, SetClipReverse> || std::is_same_v<T, SetClipPan>
                              || std::is_same_v<T, SetClipCutout>
                              || std::is_same_v<T, AddCutoutStroke> || std::is_same_v<T, SetClipKey>
                              || std::is_same_v<T, ClearClipKey> || std::is_same_v<T, ClearClipKeys>
                              || std::is_same_v<T, SetEffectKey>
                              || std::is_same_v<T, ClearEffectKey>
                              || std::is_same_v<T, ClearEffectKeys>
                              || std::is_same_v<T, SetClipSpeedCurve>
                              || std::is_same_v<T, SetClipTransform>) {
                    return apply_properties(command, project, mint, undo);
                }

                if constexpr (std::is_same_v<T, DetachAudio> || std::is_same_v<T, ReattachAudio>) {
                    return apply_audio(command, project, mint, undo);
                }

                if constexpr (std::is_same_v<T, AddTrack> || std::is_same_v<T, RemoveTrack>
                              || std::is_same_v<T, SetTrackFlag>) {
                    return apply_tracks(command, project, mint, undo);
                }

                if constexpr (std::is_same_v<T, AddTimeline> || std::is_same_v<T, SetTimelineVideo>
                              || std::is_same_v<T, RemoveTimeline>
                              || std::is_same_v<T, RenameTimeline>
                              || std::is_same_v<T, SelectTimeline>
                              || std::is_same_v<T, MoveTimeline>) {
                    return apply_timelines(command, project, mint, undo);
                }

                // `Batch` is handled above; every other alternative is routed to
                // a group.
                std::unreachable();
            },
            command.value);
}

} // namespace genesis::project
