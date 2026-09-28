// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-project undo model (editor.rs),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "project/Undo.h"

#include <algorithm>
#include <utility>

namespace genesis::project {

bool History::push(HistoryEntry entry)
{
    // Coalesce when this step names the same gesture group as the last
    // recorded step: keep the earlier before-image, so undo lands where the
    // gesture began.
    if (entry.gesture && !undo_.empty() && undo_.back().gesture == entry.gesture) {
        // Take the new command. Redo replays the stored command against the
        // restored before-image, so it must be the gesture's *last* step, not
        // its first - otherwise a redo lands in the middle of the drag.
        undo_.back().command = std::move(entry.command);
        return false;
    }
    undo_.push_back(std::move(entry));
    if (undo_.size() > DEPTH) {
        undo_.pop_front();
    }
    redo_.clear();
    bump_generation();
    return true;
}

std::optional<HistoryEntry> History::undo()
{
    if (undo_.empty()) {
        return std::nullopt;
    }
    HistoryEntry entry = std::move(undo_.back());
    undo_.pop_back();
    redo_.push_back(entry);
    return entry;
}

std::optional<HistoryEntry> History::redo()
{
    if (redo_.empty()) {
        return std::nullopt;
    }
    HistoryEntry entry = std::move(redo_.back());
    redo_.pop_back();
    // A redo is a fresh step: drop the gesture so a later same-named step
    // starts a new entry instead of coalescing into this one.
    entry.gesture.reset();
    undo_.push_back(entry);
    return entry;
}

void History::end_gesture()
{
    if (!undo_.empty()) {
        undo_.back().gesture.reset();
    }
}

// Undo walks a record in reverse - the inverse of the order `apply` filled
// it - so each vector is visited back to front, and the five vectors in the
// reverse of their declaration order: fonts, media, timelines, then tracks
// and clips. That last ordering is what lets a re-inserted track or clip
// land in a timeline that already exists. Per id the restore is idempotent;
// order matters only when re-inserting entities the edit removed, and
// reversing there keeps their relative order closest to the original -
// snapshots carry no sibling index, so re-insertion appends, and a track or
// clip whose timeline the snapshot cannot name lands on the active timeline,
// the one the command layer acts on and whose id is restored above.
void restore_before(Project &project, IdMinter &mint, const UndoRecord &record)
{
    mint.set_counter(record.id_counter);
    project.active_timeline_id = record.active_timeline_id;

    // Fonts, keyed by family rather than an id.
    for (auto it = record.fonts.rbegin(); it != record.fonts.rend(); ++it) {
        const FontSnapshot &snap = *it;
        const auto found =
                std::find_if(project.fonts.begin(), project.fonts.end(),
                             [&snap](const CustomFont &font) { return font.family == snap.id; });
        if (found != project.fonts.end()) {
            if (snap.before) {
                *found = *snap.before;
            } else {
                project.fonts.erase(found);
            }
        } else if (snap.before) {
            project.fonts.push_back(*snap.before);
        }
    }

    for (auto it = record.media.rbegin(); it != record.media.rend(); ++it) {
        const MediaSnapshot &snap = *it;
        const auto found =
                std::find_if(project.media.begin(), project.media.end(),
                             [&snap](const MediaItem &item) { return item.id == snap.id; });
        if (found != project.media.end()) {
            if (snap.before) {
                *found = *snap.before;
            } else {
                project.media.erase(found);
            }
        } else if (snap.before) {
            project.media.push_back(*snap.before);
        }
    }

    for (auto it = record.timelines.rbegin(); it != record.timelines.rend(); ++it) {
        const TimelineSnapshot &snap = *it;
        const auto found =
                std::find_if(project.timelines.begin(), project.timelines.end(),
                             [&snap](const Timeline &timeline) { return timeline.id == snap.id; });
        if (found != project.timelines.end()) {
            if (snap.before) {
                *found = *snap.before;
            } else {
                project.timelines.erase(found);
            }
        } else if (snap.before) {
            project.timelines.push_back(*snap.before);
        }
    }

    for (auto it = record.tracks.rbegin(); it != record.tracks.rend(); ++it) {
        const TrackSnapshot &snap = *it;
        bool found = false;
        for (Timeline &timeline : project.timelines) {
            const auto track_it =
                    std::find_if(timeline.tracks.begin(), timeline.tracks.end(),
                                 [&snap](const Track &track) { return track.id == snap.id; });
            if (track_it == timeline.tracks.end()) {
                continue;
            }
            found = true;
            if (snap.before) {
                *track_it = *snap.before;
            } else {
                timeline.tracks.erase(track_it);
            }
            break;
        }
        if (!found && snap.before) {
            project.active_mut().tracks.push_back(*snap.before);
        }
    }

    for (auto it = record.clips.rbegin(); it != record.clips.rend(); ++it) {
        const ClipSnapshot &snap = *it;
        bool found = false;
        for (Timeline &timeline : project.timelines) {
            const auto clip_it =
                    std::find_if(timeline.clips.begin(), timeline.clips.end(),
                                 [&snap](const Clip &clip) { return clip.id == snap.id; });
            if (clip_it == timeline.clips.end()) {
                continue;
            }
            found = true;
            if (snap.before) {
                *clip_it = *snap.before;
            } else {
                timeline.clips.erase(clip_it);
            }
            break;
        }
        if (!found && snap.before) {
            project.active_mut().clips.push_back(*snap.before);
        }
    }
}

} // namespace genesis::project
