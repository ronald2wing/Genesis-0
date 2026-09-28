// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Audio commands: detaching and reattaching a video clip's sound.

#pragma once

#include <string>

namespace genesis::project {

// Pulls a video clip's sound out into its own audio clip on a free
// lane (minting one if none is free), muting the video and moving its
// audio filters to the sound. A no-op unless the clip is an unmuted
// video whose media has audio and is not already detached.
struct DetachAudio
{
    // The video clip to detach from.
    std::string clip_id;

    bool operator==(const DetachAudio &) const = default;
};

// Undoes a detach: deletes the detached sound clip(s), unmutes the
// video and hands the sound's filters back. Accepts either the video's
// id or the sound's; a no-op when either side is gone.
struct ReattachAudio
{
    // The video clip - or its detached sound.
    std::string clip_id;

    bool operator==(const ReattachAudio &) const = default;
};

} // namespace genesis::project
