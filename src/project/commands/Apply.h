// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "project/command/Command.h"
#include "project/command/Error.h"
#include "project/IdMinter.h"
#include "project/Project.h"
#include "project/Undo.h"
#include "project/model/Clip.h"
#include "project/model/Font.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"

namespace genesis::project {

using ApplyResult = std::expected<Outcome, CommandError>;

// One arm per verb group. `apply` routes each command variant to its group;
// these are declared here so every later group file matches one signature.
ApplyResult apply_media(const Command &, Project &, IdMinter &, UndoRecord &);
ApplyResult apply_clips(const Command &, Project &, IdMinter &, UndoRecord &);
ApplyResult apply_properties(const Command &, Project &, IdMinter &, UndoRecord &);
ApplyResult apply_audio(const Command &, Project &, IdMinter &, UndoRecord &);
ApplyResult apply_tracks(const Command &, Project &, IdMinter &, UndoRecord &);
ApplyResult apply_timelines(const Command &, Project &, IdMinter &, UndoRecord &);

// The dispatcher: routes one command to its verb group, guards non-finite
// values, and makes a Batch atomic. Defined in Apply.cpp.
ApplyResult apply(const Command &, Project &, IdMinter &, UndoRecord &);

// Before-image helpers. Every verb group records an entity's pre-edit state
// before mutating it, so undo can restore it exactly. `record_created` marks
// an entity absent before the edit (undo removes it); the tag parameter is
// the entity's type, since a created entity has no value to overload on.

// Records a track's before-image. Call before mutating the track.
inline void record_before(UndoRecord &undo, const Track &before)
{
    undo.tracks.push_back(TrackSnapshot{ before.id, before });
}
// Records that a track is newly created: undo removes it.
inline void record_created(UndoRecord &undo, std::string_view id, std::in_place_type_t<Track>)
{
    undo.tracks.push_back(TrackSnapshot{ std::string(id), std::nullopt });
}

// Records a clip's before-image. Call before mutating the clip.
inline void record_before(UndoRecord &undo, const Clip &before)
{
    undo.clips.push_back(ClipSnapshot{ before.id, before });
}
// Records that a clip is newly created: undo removes it.
inline void record_created(UndoRecord &undo, std::string_view id, std::in_place_type_t<Clip>)
{
    undo.clips.push_back(ClipSnapshot{ std::string(id), std::nullopt });
}

// Records a timeline's before-image. Call before mutating the timeline.
inline void record_before(UndoRecord &undo, const Timeline &before)
{
    undo.timelines.push_back(TimelineSnapshot{ before.id, before });
}
// Records that a timeline is newly created: undo removes it.
inline void record_created(UndoRecord &undo, std::string_view id, std::in_place_type_t<Timeline>)
{
    undo.timelines.push_back(TimelineSnapshot{ std::string(id), std::nullopt });
}

// Records a media item's before-image. Call before mutating the item.
inline void record_before(UndoRecord &undo, const MediaItem &before)
{
    undo.media.push_back(MediaSnapshot{ before.id, before });
}
// Records that a media item is newly created: undo removes it.
inline void record_created(UndoRecord &undo, std::string_view id, std::in_place_type_t<MediaItem>)
{
    undo.media.push_back(MediaSnapshot{ std::string(id), std::nullopt });
}

// Records a font's before-image. Call before mutating the font. A font is
// keyed by its family name, not an id, so the snapshot id is the family.
inline void record_before(UndoRecord &undo, const CustomFont &before)
{
    undo.fonts.push_back(FontSnapshot{ before.family, before });
}
// Records that a font is newly created: undo removes it.
inline void record_created(UndoRecord &undo, std::string_view id, std::in_place_type_t<CustomFont>)
{
    undo.fonts.push_back(FontSnapshot{ std::string(id), std::nullopt });
}

} // namespace genesis::project
