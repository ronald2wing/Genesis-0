// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The Project edit model: everything a document stores except app-level settings.

#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "project/model/Font.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"
#include "project/model/VideoSettings.h"

namespace genesis::project {

// The edit: everything the document stores except the app-level settings
// (name, output format) that the host manages around it.
// On disk: camelCase.
struct Project
{
    // The bin: every imported file, shared by all timelines.
    std::vector<MediaItem> media;
    // Fonts the user added from disk, available to every title.
    std::vector<CustomFont> fonts;
    // Every timeline, in tab order. Always at least one.
    std::vector<Timeline> timelines;
    // Which timeline commands act on. Maintained by the command layer, so
    // it always names a member of `timelines`; `active` degrades to the
    // first timeline if it somehow does not.
    std::string active_timeline_id;
    // The document layer (src/project/document/**) is fully implemented and
    // exposes no `extra` member: unknown document fields are dropped on load,
    // not kept for a round-trip. Timeline, Track, Clip, and MediaItem carry no
    // such placeholder either.

    // Builds the founding timeline around `video`: the one place the
    // starter lanes "T1".."T4" are minted. Private so it never re-enters
    // the public constructors.
    explicit Project(VideoSettings video)
    {
        Timeline timeline;
        timeline.id = "TL1";
        timeline.name = "Timeline 1";
        timeline.video = std::move(video);
        for (int number = 1; number <= 4; ++number) {
            Track track;
            track.id = "T" + std::to_string(number);
            timeline.tracks.push_back(std::move(track));
        }
        timelines.push_back(std::move(timeline));
        active_timeline_id = "TL1";
    }

    // A new project: one timeline, four lanes, at the default frame.
    Project() : Project(VideoSettings{ }) { }

    // A new project whose one timeline renders to `video` - what a project
    // created from the launch screen's size and rate pickers starts as.
    static Project with_video(VideoSettings video) { return Project(std::move(video)); }

    // The timeline being edited. The active id is maintained by the command
    // layer, so a broken invariant here is a bug, not user input - but it
    // degrades to the first timeline rather than panicking.
    const Timeline &active() const
    {
        const auto it =
                std::find_if(timelines.begin(), timelines.end(), [this](const Timeline &timeline) {
                    return timeline.id == active_timeline_id;
                });
        return it != timelines.end() ? *it : timelines.front();
    }

    // Mutable twin of `active`, with the same degrade-to-first behaviour.
    Timeline &active_mut()
    {
        const auto it =
                std::find_if(timelines.begin(), timelines.end(), [this](const Timeline &timeline) {
                    return timeline.id == active_timeline_id;
                });
        return it != timelines.end() ? *it : timelines.front();
    }

    // The bin entry with this id, or None if it was removed.
    const MediaItem *media_by_id(std::string_view media_id) const
    {
        const auto it = std::find_if(media.begin(), media.end(), [media_id](const MediaItem &item) {
            return item.id == media_id;
        });
        return it != media.end() ? &*it : nullptr;
    }

    // Returns every media item whose file does not exist on disk.
    std::vector<MissingMedia> missing_media() const
    {
        std::vector<MissingMedia> missing;
        for (const MediaItem &item : media) {
            if (!std::filesystem::exists(std::filesystem::path(item.path))) {
                missing.push_back(MissingMedia{ item.id, item.name, item.path });
            }
        }
        return missing;
    }

    bool operator==(const Project &) const = default;
};

} // namespace genesis::project
