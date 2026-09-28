// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Timeline model: a named set of lanes and clips.

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/ClipTimeline.h"

#include "project/model/Clip.h"
#include "project/model/Track.h"
#include "project/model/VideoSettings.h"

namespace genesis::project {

using genesis::core::Rational;

// One timeline: a name and its lanes and clips. Every operation takes the
// project and works on whichever timeline is active.
// On disk: camelCase.
struct Timeline
{
    // Minted by the editor ("tl2", ...), except the founding "TL1".
    std::string id;
    // The tab label; "Timeline N" by default, renameable but never blank.
    std::string name = "Timeline";
    // The frame this timeline renders to and the rate it runs at. Each
    // timeline's own: a vertical cut for one platform and a wide one for
    // another are different frames of the same media, and that is most of
    // what a second timeline is for.
    VideoSettings video;
    // The lanes, top to bottom. Never empty - `RemoveTrack` keeps a floor
    // of one.
    std::vector<Track> tracks;
    // Every clip on this timeline, in insertion order, not time order -
    // readers must sort by `start` where order matters.
    std::vector<Clip> clips;

    Timeline() = default;

    // The clip with this id, or None if it is not on this timeline - which
    // most commands treat as a tolerated no-op, not an error.
    const Clip *clip(std::string_view clip_id) const
    {
        const auto it = std::find_if(clips.begin(), clips.end(),
                                     [clip_id](const Clip &clip) { return clip.id == clip_id; });
        return it != clips.end() ? &*it : nullptr;
    }

    // Mutable twin of `clip`. The clip is copied out of any undo snapshot
    // still sharing it before the reference is handed back.
    Clip *clip_mut(std::string_view clip_id)
    {
        const auto it = std::find_if(clips.begin(), clips.end(),
                                     [clip_id](const Clip &clip) { return clip.id == clip_id; });
        return it != clips.end() ? &*it : nullptr;
    }

    // Mutable access to the clip at `index`, copied out of any snapshot
    // sharing it first. The `Vec` index twin of `clip_mut`.
    Clip &clip_at_mut(std::size_t index) { return clips[index]; }

    // The clips `keep` picks, mutable, each copied out of any snapshot
    // sharing it only once picked: a ripple that moves ten clips of five
    // thousand copies ten, and the undo step stays the size of the edit
    // (audit 2026-09-23, #8).
    template <typename Pred>
    std::vector<Clip *> clips_where(Pred keep)
    {
        std::vector<Clip *> picked;
        for (Clip &clip : clips) {
            if (keep(clip)) {
                picked.push_back(&clip);
            }
        }
        return picked;
    }

    // The track with this id, or None if it was removed.
    const Track *track(std::string_view track_id) const
    {
        const auto it = std::find_if(tracks.begin(), tracks.end(), [track_id](const Track &track) {
            return track.id == track_id;
        });
        return it != tracks.end() ? &*it : nullptr;
    }

    // Where the last clip ends. Zero for an empty timeline.
    Rational duration() const
    {
        Rational end = Rational::ZERO;
        for (const Clip &clip : clips) {
            end = std::max(end, clip.start + clip.duration);
        }
        return end;
    }

    bool operator==(const Timeline &) const = default;
};

} // namespace genesis::project
