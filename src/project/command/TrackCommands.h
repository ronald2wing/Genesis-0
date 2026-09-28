// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Track commands: adding, removing and toggling lanes.

#pragma once

#include <string>

namespace genesis::project {

// Which per-track toggle a Command::SetTrackFlag flips.
// On disk: lowercase.
enum class TrackFlag {
    // Track::visible: whether the track's video reaches the composite.
    Visible,
    // Track::muted: whether the track's audio is silenced.
    Muted,
};

// Appends a lane named after the highest "Track N" in use, minting a
// "t" id.
struct AddTrack
{
    bool operator==(const AddTrack &) const = default;
};

// Deletes a lane and every clip on it. Errs at the floor of one track.
struct RemoveTrack
{
    // The lane to delete.
    std::string track_id;

    bool operator==(const RemoveTrack &) const = default;
};

// Flips one of a track's two toggles. An unknown id is a no-op.
struct SetTrackFlag
{
    // The lane to change.
    std::string track_id;
    // Which toggle: visibility or mute.
    TrackFlag flag;
    // The new setting.
    bool value;

    bool operator==(const SetTrackFlag &) const = default;
};

} // namespace genesis::project
