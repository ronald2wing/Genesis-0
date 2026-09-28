// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/commands/mod.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Timeline commands: adding, removing, renaming, selecting and reordering timelines.

#pragma once

#include <cstddef>
#include <string>

#include "project/model/VideoSettings.h"

namespace genesis::project {

// Adds a fresh timeline - four new lanes, "Timeline N" after the
// highest in use - and makes it active. Mints a "tl" id.
struct AddTimeline
{
    bool operator==(const AddTimeline &) const = default;
};

// Deletes a timeline, moving the active tab to a neighbour if it was
// this one. Errs at the floor of one timeline.
struct RemoveTimeline
{
    // The timeline to delete. An unknown id is a no-op.
    std::string timeline_id;

    bool operator==(const RemoveTimeline &) const = default;
};

// Sets a timeline's output frame and rate. A term no frame could have,
// such as a zero dimension or a zero rate, is refused as a no-op rather
// than clamped: there is no nearest real size to a zero.
struct SetTimelineVideo
{
    // The timeline. An unknown id is a no-op.
    std::string timeline_id;
    // The frame and the rate, together.
    VideoSettings video;

    bool operator==(const SetTimelineVideo &) const = default;
};

// Renames a timeline tab. Whitespace-only names are ignored so a tab
// can never end up blank; unknown ids are tolerated.
struct RenameTimeline
{
    // The timeline to rename.
    std::string timeline_id;
    // The new tab label; trimmed before it lands.
    std::string name;

    bool operator==(const RenameTimeline &) const = default;
};

// Switches which timeline subsequent commands act on. An unknown id
// leaves the selection where it was.
struct SelectTimeline
{
    // The timeline to switch to.
    std::string timeline_id;

    bool operator==(const SelectTimeline &) const = default;
};

// Moves a timeline tab to a new position in the strip. Which timeline
// is active does not change - only the order of the tabs.
struct MoveTimeline
{
    // The timeline to move. An unknown id is a no-op.
    std::string timeline_id;
    // Where it lands among its siblings, 0-based, counted with the
    // timeline already removed from its old slot. Clamped to the end.
    std::size_t index;

    bool operator==(const MoveTimeline &) const = default;
};

} // namespace genesis::project
