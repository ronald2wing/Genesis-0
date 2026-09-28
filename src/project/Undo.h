// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Derived from Concat's concat-project undo model (editor.rs),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "project/command/Command.h"
#include "project/IdMinter.h"
#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Font.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"

namespace genesis::project {

// One entity's before-image. `before` nullopt means the entity was absent
// before the edit (undo removes it); a value means it was present (undo
// replaces or re-inserts it).
struct ClipSnapshot
{
    std::string id;
    std::optional<Clip> before;

    bool operator==(const ClipSnapshot &) const = default;
};

struct TrackSnapshot
{
    std::string id;
    std::optional<Track> before;

    bool operator==(const TrackSnapshot &) const = default;
};

struct TimelineSnapshot
{
    std::string id;
    std::optional<Timeline> before;

    bool operator==(const TimelineSnapshot &) const = default;
};

struct MediaSnapshot
{
    std::string id;
    std::optional<MediaItem> before;

    bool operator==(const MediaSnapshot &) const = default;
};

struct FontSnapshot
{
    std::string id;
    std::optional<CustomFont> before;

    bool operator==(const FontSnapshot &) const = default;
};

// Everything undo needs to reverse one edit: the counter and the active
// timeline as they were, plus the before-images of every entity the edit
// touched.
struct UndoRecord
{
    // The IdMinter counter before the edit.
    std::uint64_t id_counter = 0;
    // The active timeline id before the edit.
    std::string active_timeline_id;
    std::vector<ClipSnapshot> clips;
    std::vector<TrackSnapshot> tracks;
    std::vector<TimelineSnapshot> timelines;
    std::vector<MediaSnapshot> media;
    std::vector<FontSnapshot> fonts;

    bool operator==(const UndoRecord &) const = default;
};

// Restores every entity in `record` to its before-image: a snapshot with a
// value replaces (or re-inserts) the entity with that id; a snapshot with
// nullopt removes the entity with that id (it was created by the edit).
// Also restores the IdMinter counter and the active timeline id. This is the
// inverse the History applies on undo, and the revert a failed Batch uses.
void restore_before(Project &project, IdMinter &mint, const UndoRecord &record);

// One undo step: the command that made the edit (replayed on redo) and the
// before-images that reverse it. `gesture`, when named, groups a run of
// small edits into one coalesced step.
struct HistoryEntry
{
    // The forward edit, replayed on redo.
    Command command{ Batch{ } };
    // The inverse: before-images.
    UndoRecord before;
    // The coalescing group, if named.
    std::optional<std::string> gesture;
};

// The undo/redo stacks and the dirty tracking.
class History
{
public:
    static constexpr std::size_t DEPTH = 200;

    // Records a step; evicts the oldest when deeper than DEPTH; bumps the
    // generation; clears redo. If `gesture` names the same group as the last
    // recorded step, coalesce instead: return without pushing (the caller
    // keeps the earlier before-image, so undo lands where the gesture began).
    // Returns true when a new entry was pushed, false when coalesced.
    bool push(HistoryEntry entry);

    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }

    // Move the top entry to the other stack and return it; nullopt if empty.
    // The CALLER performs the project mutation using the returned entry.
    std::optional<HistoryEntry> undo();
    std::optional<HistoryEntry> redo();

    // Clears the last entry's gesture.
    void end_gesture();

    // Dirty tracking: generation bumps on every recorded step; mark_saved
    // records the current generation.
    std::uint64_t generation() const { return generation_; }
    void bump_generation() { ++generation_; }
    void mark_saved() { saved_generation_ = generation_; }
    bool dirty() const { return generation_ != saved_generation_; }

private:
    // Oldest at the front.
    std::deque<HistoryEntry> undo_;
    std::deque<HistoryEntry> redo_;
    std::uint64_t generation_ = 0;
    std::uint64_t saved_generation_ = 0;
};

} // namespace genesis::project
