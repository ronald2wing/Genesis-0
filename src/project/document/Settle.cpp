// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The document's parse and settle pass: a JSON object to a sound project.

#include <set>

#include "project/Naming.h"
#include "project/document/Internal.h"

namespace genesis::project::detail {

std::optional<Project> parse_project(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Project project;
    // A parsed project starts empty: the founding timeline is for a new
    // project, not for one read back from disk.
    project.media.clear();
    project.fonts.clear();
    project.timelines.clear();
    project.active_timeline_id.clear();
    if (const auto it = j.find("media"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<MediaItem> item = read_media_item(entry)) {
                project.media.push_back(std::move(*item));
            }
        }
    }
    if (const auto it = j.find("fonts"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<CustomFont> font = read_font(entry)) {
                project.fonts.push_back(std::move(*font));
            }
        }
    }
    if (const auto it = j.find("timelines"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<Timeline> timeline = read_timeline(entry)) {
                project.timelines.push_back(std::move(*timeline));
            }
        }
    }
    if (const auto it = j.find("activeTimelineId"); it != j.end()) {
        if (!it->is_string()) {
            return std::nullopt;
        }
        project.active_timeline_id = it->get<std::string>();
    }
    return project;
}

std::optional<Clip> settle_clip(Clip clip, const std::vector<Track> &tracks,
                                const std::vector<MediaItem> &media)
{
    if (clip.id.empty()) {
        return std::nullopt;
    }
    const bool on_track = std::any_of(tracks.begin(), tracks.end(), [&clip](const Track &track) {
        return track.id == clip.track_id;
    });
    if (!on_track) {
        return std::nullopt;
    }
    if (clip.kind == ClipKind::Text || clip.kind == ClipKind::Layer) {
        clip.media_id.clear();
    } else {
        const bool has_media =
                std::any_of(media.begin(), media.end(),
                            [&clip](const MediaItem &item) { return item.id == clip.media_id; });
        if (!has_media) {
            return std::nullopt;
        }
    }
    if (clip.kind != ClipKind::Text) {
        clip.text.reset();
    }
    return clip.tidy();
}

std::optional<Timeline> settle_timeline(Timeline timeline, const std::vector<MediaItem> &media)
{
    if (timeline.id.empty()) {
        return std::nullopt;
    }
    if (trim(timeline.name).empty()) {
        timeline.name = "Timeline";
    }
    timeline.video = timeline.video.or_fallback(VideoSettings{ });
    // Drop tracks without an id and duplicates, first one winning.
    std::vector<Track> tracks;
    std::set<std::string> seen;
    for (Track &track : timeline.tracks) {
        if (!track.id.empty() && seen.insert(track.id).second) {
            tracks.push_back(std::move(track));
        }
    }
    timeline.tracks = std::move(tracks);
    if (timeline.tracks.empty()) {
        return std::nullopt;
    }
    std::vector<Clip> clips;
    clips.reserve(timeline.clips.size());
    for (Clip &clip : timeline.clips) {
        if (std::optional<Clip> settled = settle_clip(std::move(clip), timeline.tracks, media)) {
            clips.push_back(std::move(*settled));
        }
    }
    timeline.clips = std::move(clips);
    return timeline;
}

std::optional<Project> settle_project(Project project)
{
    std::vector<MediaItem> media;
    media.reserve(project.media.size());
    for (MediaItem &item : project.media) {
        if (!item.id.empty()) {
            media.push_back(item.tidy());
        }
    }
    project.media = std::move(media);

    std::erase_if(project.fonts,
                  [](const CustomFont &font) { return font.family.empty() || font.path.empty(); });

    std::vector<Timeline> timelines;
    std::set<std::string> seen;
    for (Timeline &timeline : project.timelines) {
        if (std::optional<Timeline> settled = settle_timeline(std::move(timeline), project.media)) {
            if (seen.insert(settled->id).second) {
                timelines.push_back(std::move(*settled));
            }
        }
    }
    project.timelines = std::move(timelines);
    if (project.timelines.empty()) {
        return std::nullopt;
    }

    const bool active_exists = std::any_of(project.timelines.begin(), project.timelines.end(),
                                           [&project](const Timeline &timeline) {
                                               return timeline.id == project.active_timeline_id;
                                           });
    if (!active_exists) {
        project.active_timeline_id = project.timelines[0].id;
    }
    return project;
}

} // namespace genesis::project::detail
