// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "workspace/Templates.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "project/Project.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/command/TimelineCommands.h"
#include "project/command/TrackCommands.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/Track.h"
#include "project/Document.h"
#include "project/Editor.h"
#include "core/JsonGuard.h"

namespace genesis::workspace {

namespace core = genesis::core;

namespace {

// A probed media item as the command layer wants it: every field the model
// keeps except the id and the placeholder flag. The id is minted on import;
// the placeholder belongs to a packed template bundle, which is not what a
// saved cut is.
project::NewMedia to_new_media(const project::MediaItem &item)
{
    project::NewMedia out;
    out.path = item.path;
    out.name = item.name;
    out.duration = item.duration;
    out.kind = item.kind;
    out.width = item.width;
    out.height = item.height;
    out.frame_rate = item.frame_rate;
    out.frame_rate_fraction = item.frame_rate_fraction;
    out.video_codec = item.video_codec;
    out.audio_codec = item.audio_codec;
    out.has_audio = item.has_audio;
    out.audio_tracks = item.audio_tracks;
    out.origin = item.origin;
    out.color_space = item.color_space;
    return out;
}

// The bin item already holding `path`, or null - the thing a duplicate import
// is a no-op against.
const project::MediaItem *media_by_path(const project::Project &project, std::string_view path)
{
    const auto it =
            std::find_if(project.media.begin(), project.media.end(),
                         [path](const project::MediaItem &item) { return item.path == path; });
    return it != project.media.end() ? &*it : nullptr;
}

// The length a freshly placed clip of this media gets: the probed duration,
// five seconds for a still or one the container did not report.
project::Rational default_duration(const project::MediaItem &media)
{
    return media.kind == project::MediaKind::Image
            ? project::Rational{ 5, 1 }
            : media.duration.value_or(project::Rational{ 5, 1 });
}

} // namespace

std::expected<TemplateInfo, std::string> save_template(const project::Project &source,
                                                       const std::vector<std::string> &clip_ids,
                                                       std::string_view name,
                                                       const std::filesystem::path &file)
{
    const project::Timeline &active = source.active();

    // The selection, deduplicated and held to clips actually on the active
    // timeline; an id naming another timeline is skipped.
    std::vector<const project::Clip *> selected;
    std::set<std::string> seen;
    for (const std::string &id : clip_ids) {
        if (!seen.insert(id).second) {
            continue;
        }
        if (const project::Clip *clip = active.clip(id)) {
            selected.push_back(clip);
        }
    }
    if (selected.empty()) {
        return std::unexpected{ std::string{ "No selected clips on the active timeline." } };
    }

    // A title or a layer has no media behind it, so a template could not
    // place it back. Refuse rather than thin the selection silently.
    for (const project::Clip *clip : selected) {
        if (clip->media_id.empty() || source.media_by_id(clip->media_id) == nullptr) {
            return std::unexpected{ std::string{ "Every selected clip needs media behind it." } };
        }
    }

    // The lanes the selection occupies, in the timeline's own top-to-bottom
    // order - what "lane fidelity" means, since a track has no name.
    std::vector<const project::Track *> lanes;
    for (const project::Track &track : active.tracks) {
        const bool occupied =
                std::any_of(selected.begin(), selected.end(), [&track](const project::Clip *clip) {
                    return clip->track_id == track.id;
                });
        if (occupied) {
            lanes.push_back(&track);
        }
    }

    project::Project slice;
    slice.media.clear();
    slice.fonts.clear();
    slice.timelines.clear();

    // Only the media the selection references, in bin order, untouched.
    std::set<std::string> wanted;
    for (const project::Clip *clip : selected) {
        wanted.insert(clip->media_id);
    }
    for (const project::MediaItem &item : source.media) {
        if (wanted.contains(item.id)) {
            slice.media.push_back(item);
        }
    }

    project::Timeline timeline;
    timeline.id = "TL1";
    timeline.name = "Timeline 1";
    timeline.video = active.video;
    for (const project::Track *track : lanes) {
        timeline.tracks.push_back(*track);
    }
    // Each clip stripped to the placement the contract promises: media, lane,
    // start, duration, kind and name. Nothing else rides along.
    for (const project::Track *track : lanes) {
        std::vector<const project::Clip *> lane_clips;
        for (const project::Clip *clip : selected) {
            if (clip->track_id == track->id) {
                lane_clips.push_back(clip);
            }
        }
        std::sort(
                lane_clips.begin(), lane_clips.end(),
                [](const project::Clip *a, const project::Clip *b) { return a->start < b->start; });
        for (const project::Clip *clip : lane_clips) {
            project::Clip stripped = project::Clip::blank(clip->id, clip->track_id, clip->kind,
                                                          clip->name, clip->start, clip->duration);
            stripped.media_id = clip->media_id;
            timeline.clips.push_back(std::move(stripped));
        }
    }
    slice.timelines.push_back(std::move(timeline));
    slice.active_timeline_id = "TL1";

    project::DocumentSettings settings;
    settings.name = std::string(name);
    const nlohmann::json document = project::to_document(slice, settings);

    std::ofstream out(file);
    if (!out) {
        return std::unexpected{ std::string{ "Cannot write the template file." } };
    }
    out << document.dump(2) << '\n';
    if (!out) {
        return std::unexpected{ std::string{ "Cannot write the template file." } };
    }

    return TemplateInfo{ std::string(name), slice.media.size(), lanes.size(), selected.size() };
}

std::expected<std::string, std::string> instantiate_template(project::Editor &editor,
                                                             const std::filesystem::path &file)
{
    std::ifstream in(file);
    if (!in) {
        return std::unexpected{ std::string{ "Cannot read the template file." } };
    }
    std::string error;
    const std::optional<nlohmann::json> parsed = core::parse_document_json(in, error);
    if (!parsed) {
        return std::unexpected{ std::string{ "The template file is not a valid document." } };
    }
    const nlohmann::json &document = *parsed;
    const std::optional<project::Project> parsed_project = project::from_document(document);
    if (!parsed_project) {
        return std::unexpected{ std::string{ "The template file is not a usable project." } };
    }
    const project::Project &source = *parsed_project;
    if (source.timelines.empty()) {
        return std::unexpected{ std::string{ "The template holds no timeline." } };
    }
    const project::Timeline &template_timeline = source.active();

    // The template's media ids, remapped to the ids this editor mints. The
    // bin is shared across timelines, so a path already here reuses its item
    // rather than duplicating it.
    std::map<std::string, std::string> media_ids;
    std::map<std::string, const project::MediaItem *> media_by_id;
    for (const project::MediaItem &item : source.media) {
        media_by_id.emplace(item.id, &item);
        const auto added =
                editor.apply(project::Command{ project::AddMedia{ to_new_media(item) } });
        if (!added) {
            return std::unexpected{ std::string{ std::string(added.error().message()) } };
        }
        if (added->created_id) {
            media_ids.emplace(item.id, *added->created_id);
        } else {
            const project::MediaItem *existing = media_by_path(editor.project(), item.path);
            if (existing == nullptr) {
                return std::unexpected{ std::string{ "The template media cannot be imported." } };
            }
            media_ids.emplace(item.id, existing->id);
        }
    }

    const auto added_timeline = editor.apply(project::Command{ project::AddTimeline{ } });
    if (!added_timeline || !added_timeline->created_id) {
        return std::unexpected{ std::string{ "Cannot create the template timeline." } };
    }
    const std::string timeline_id = *added_timeline->created_id;

    // The fresh timeline's lanes, read before any is appended, so the ids are
    // copied out before the vector can reallocate. Lane i of the template maps
    // to lane i here: order is the fidelity, not names.
    std::vector<std::string> track_ids;
    for (const project::Track &track : editor.project().active().tracks) {
        track_ids.push_back(track.id);
    }
    while (track_ids.size() < template_timeline.tracks.size()) {
        const auto added_track = editor.apply(project::Command{ project::AddTrack{ } });
        if (!added_track || !added_track->created_id) {
            return std::unexpected{ std::string{ "Cannot create a template lane." } };
        }
        track_ids.push_back(*added_track->created_id);
    }

    for (std::size_t lane = 0; lane < template_timeline.tracks.size(); ++lane) {
        const std::string &template_track = template_timeline.tracks[lane].id;
        std::vector<const project::Clip *> lane_clips;
        for (const project::Clip &clip : template_timeline.clips) {
            if (clip.track_id == template_track) {
                lane_clips.push_back(&clip);
            }
        }
        std::sort(
                lane_clips.begin(), lane_clips.end(),
                [](const project::Clip *a, const project::Clip *b) { return a->start < b->start; });
        for (const project::Clip *clip : lane_clips) {
            const auto id_it = media_ids.find(clip->media_id);
            if (id_it == media_ids.end()) {
                continue; // the reader already drops such clips; be safe.
            }
            const auto placed = editor.apply(project::Command{
                    project::AddClip{ id_it->second, track_ids[lane], clip->start, false } });
            if (!placed || !placed->created_id) {
                return std::unexpected{ std::string{ "Cannot place a template clip." } };
            }
            const std::string clip_id = *placed->created_id;
            const project::MediaItem *media = media_by_id.at(clip->media_id);

            // AddClip names and lengths the clip from the media; restore the
            // template's own where the user changed them.
            if (clip->name != media->name) {
                project::ClipPatch patch;
                patch.name = clip->name;
                const auto renamed = editor.apply(
                        project::Command{ project::UpdateClip{ clip_id, std::move(patch) } });
                if (!renamed) {
                    return std::unexpected{ std::string{ std::string(renamed.error().message()) } };
                }
            }
            const project::Rational produced = default_duration(*media);
            if (clip->duration != produced) {
                const auto trimmed = editor.apply(project::Command{ project::TrimClip{
                        clip_id, project::TrimEdge::End, clip->duration - produced, false } });
                if (!trimmed) {
                    return std::unexpected{ std::string{ std::string(trimmed.error().message()) } };
                }
            }
        }
    }

    return timeline_id;
}

} // namespace genesis::workspace
