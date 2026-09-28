// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-project editor (editor.rs),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "project/Editor.h"

#include <algorithm>
#include <map>
#include <utility>

#include "project/command/Validate.h"

namespace genesis::project {

void Editor::load(Project project)
{
    project_ = std::move(project);
    // The mint restarts from zero and adopts every id the restored project
    // already uses, so the next minted id cannot collide with the file.
    mint_.set_counter(0);
    mint_.adopt_project(project_);
    // A freshly loaded project has no undo history and is not dirty.
    history_ = History{ };
}

ApplyResult Editor::apply(const Command &command)
{
    return apply_within(std::nullopt, command);
}

ApplyResult Editor::apply_within(const std::optional<std::string> &gesture, const Command &command)
{
    // View state is saved, not undoable: apply it and discard the record,
    // bumping the generation so the document knows something changed.
    if (is_view_state(command)) {
        UndoRecord throwaway;
        ApplyResult result = ::genesis::project::apply(command, project_, mint_, throwaway);
        if (result) {
            history_.bump_generation();
        }
        return result;
    }

    // A normal edit: snapshot the counter and the active timeline before
    // the command runs, so undo can restore them exactly.
    UndoRecord record;
    record.id_counter = mint_.counter();
    record.active_timeline_id = project_.active_timeline_id;

    ApplyResult result = apply_and_derive(command, record);
    if (!result || !result->applied) {
        return result;
    }

    history_.push(HistoryEntry{ command, std::move(record), gesture });
    return result;
}

ApplyResult Editor::apply_and_derive(const Command &command, UndoRecord &record)
{
    ApplyResult result = ::genesis::project::apply(command, project_, mint_, record);
    if (!result) {
        return result;
    }
    if (!result->applied) {
        return result;
    }

    // The derived steps - tidying the clips the command wrote and promoting
    // any timeline that just took its first HDR clip - run as part of this
    // same step, so one undo takes them back with the edit.
    std::vector<std::string> written_clips;
    for (const ClipSnapshot &snap : record.clips) {
        written_clips.push_back(snap.id);
    }
    std::vector<std::string> written_tracks;
    for (const TrackSnapshot &snap : record.tracks) {
        written_tracks.push_back(snap.id);
    }
    std::vector<std::string> written_timelines;
    for (const TimelineSnapshot &snap : record.timelines) {
        written_timelines.push_back(snap.id);
    }
    // A timeline is written when the command wrote it directly, or wrote any
    // of its clips or lanes.
    for (const Timeline &timeline : project_.timelines) {
        const bool wrote_clip = std::any_of(
                timeline.clips.begin(), timeline.clips.end(), [&written_clips](const Clip &clip) {
                    return std::find(written_clips.begin(), written_clips.end(), clip.id)
                            != written_clips.end();
                });
        const bool wrote_track = std::any_of(timeline.tracks.begin(), timeline.tracks.end(),
                                             [&written_tracks](const Track &track) {
                                                 return std::find(written_tracks.begin(),
                                                                  written_tracks.end(), track.id)
                                                         != written_tracks.end();
                                             });
        if (wrote_clip || wrote_track) {
            written_timelines.push_back(timeline.id);
        }
    }

    tidy_touched(record, written_clips);
    follow_first_hdr(record, written_timelines);
    return result;
}

void Editor::tidy_touched(UndoRecord &record, const std::vector<std::string> &written_clips)
{
    for (Timeline &timeline : project_.timelines) {
        for (Clip &clip : timeline.clips) {
            if (std::find(written_clips.begin(), written_clips.end(), clip.id)
                == written_clips.end()) {
                continue;
            }
            Clip tidied = clip.tidy();
            if (!(tidied == clip)) {
                record_before(record, clip);
                clip = std::move(tidied);
            }
        }
    }
}

void Editor::follow_first_hdr(UndoRecord &record, const std::vector<std::string> &written_timelines)
{
    // The HDR media before and after the command. Nothing to promote without
    // an HDR clip somewhere.
    std::vector<std::string> now;
    for (const MediaItem &media : project_.media) {
        if (is_hdr(media.color_space)) {
            now.push_back(media.id);
        }
    }
    if (now.empty()) {
        return;
    }

    std::map<std::string, const MediaSnapshot *> media_before;
    for (const MediaSnapshot &snap : record.media) {
        media_before.emplace(snap.id, &snap);
    }
    std::vector<std::string> then;
    for (const MediaItem &media : project_.media) {
        const auto it = media_before.find(media.id);
        if (it != media_before.end() && !it->second->before) {
            continue; // made this step: no before colour
        }
        const ColorSpace before =
                it != media_before.end() ? it->second->before->color_space : media.color_space;
        if (is_hdr(before)) {
            then.push_back(media.id);
        }
    }
    // Media removed this step keeps its before colour in `then`.
    for (const MediaSnapshot &snap : record.media) {
        if (!snap.before || !is_hdr(snap.before->color_space)) {
            continue;
        }
        const bool still_there =
                std::any_of(project_.media.begin(), project_.media.end(),
                            [&snap](const MediaItem &media) { return media.id == snap.id; });
        if (!still_there) {
            then.push_back(snap.id);
        }
    }

    // Clips the command wrote, keyed by id, so `had` reads each clip's
    // before media rather than the media it names now.
    std::map<std::string, const ClipSnapshot *> clip_before;
    for (const ClipSnapshot &snap : record.clips) {
        clip_before.emplace(snap.id, &snap);
    }
    // Clips no longer on any timeline: written this step and removed.
    std::vector<const ClipSnapshot *> removed_clips;
    std::vector<std::string> current_clip_ids;
    for (const Timeline &timeline : project_.timelines) {
        for (const Clip &clip : timeline.clips) {
            current_clip_ids.push_back(clip.id);
        }
    }
    for (const ClipSnapshot &snap : record.clips) {
        if (!snap.before) {
            continue;
        }
        if (std::find(current_clip_ids.begin(), current_clip_ids.end(), snap.id)
            == current_clip_ids.end()) {
            removed_clips.push_back(&snap);
        }
    }
    // A lane still present maps to its timeline, so a removed clip can be
    // attributed to the timeline it was cut from.
    std::map<std::string, std::string> track_timeline;
    for (const Timeline &timeline : project_.timelines) {
        for (const Track &track : timeline.tracks) {
            track_timeline.emplace(track.id, timeline.id);
        }
    }
    // Timelines the command made keep the colour they were made with.
    std::vector<std::string> made_timelines;
    for (const TimelineSnapshot &snap : record.timelines) {
        if (!snap.before) {
            made_timelines.push_back(snap.id);
        }
    }

    for (Timeline &timeline : project_.timelines) {
        if (is_hdr(timeline.video.color_space)) {
            continue;
        }
        if (std::find(made_timelines.begin(), made_timelines.end(), timeline.id)
            != made_timelines.end()) {
            continue;
        }
        if (std::find(written_timelines.begin(), written_timelines.end(), timeline.id)
            == written_timelines.end()) {
            continue;
        }
        // An HDR clip on the timeline before this step?
        bool had = std::any_of(timeline.clips.begin(), timeline.clips.end(), [&](const Clip &clip) {
            const auto it = clip_before.find(clip.id);
            if (it != clip_before.end() && !it->second->before) {
                return false; // made this step: not on the timeline before
            }
            const std::string &before_media =
                    it != clip_before.end() ? it->second->before->media_id : clip.media_id;
            return std::find(then.begin(), then.end(), before_media) != then.end();
        });
        if (!had) {
            had = std::any_of(
                    removed_clips.begin(), removed_clips.end(), [&](const ClipSnapshot *snap) {
                        const auto it = track_timeline.find(snap->before->track_id);
                        const std::string &timeline_id = it != track_timeline.end()
                                ? it->second
                                : project_.active_timeline_id;
                        return timeline_id == timeline.id
                                && std::find(then.begin(), then.end(), snap->before->media_id)
                                != then.end();
                    });
        }
        if (had) {
            continue;
        }
        const bool has =
                std::any_of(timeline.clips.begin(), timeline.clips.end(), [&now](const Clip &clip) {
                    return std::find(now.begin(), now.end(), clip.media_id) != now.end();
                });
        if (has) {
            record_before(record, timeline);
            timeline.video.color_space = ColorSpace::Hlg;
        }
    }
}

void Editor::undo()
{
    if (auto entry = history_.undo()) {
        restore_before(project_, mint_, entry->before);
    }
}

void Editor::redo()
{
    if (auto entry = history_.redo()) {
        UndoRecord rec;
        rec.id_counter = mint_.counter();
        rec.active_timeline_id = project_.active_timeline_id;
        // The undo stack already holds the before-image; this record only
        // gathers what apply writes now and is discarded, so redo re-runs the
        // same derived steps apply_within did without re-recording them.
        (void)apply_and_derive(entry->command, rec);
    }
}

} // namespace genesis::project
