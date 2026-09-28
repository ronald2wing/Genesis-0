// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/doc.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "project/Document.h"
#include "project/document/Internal.h"

namespace genesis::project {

using nlohmann::json;
using namespace detail;

std::optional<int> document_version(const nlohmann::json &doc)
{
    // A document from before the version field existed is version 1 - the
    // shape the field was first given - so absent reads as the oldest, not
    // as broken. A non-integer in the slot is the same as absent: it is not
    // a version this build can reason about, and version 1 is the default.
    const auto it = doc.find("version");
    if (it == doc.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    return static_cast<int>(it->get<std::int64_t>());
}

std::optional<Rational> decode_time(const nlohmann::json &value)
{
    // Concat's wire format stored times as f64 seconds; the host stores the
    // exact fraction. Read both so a document written by either build opens.
    if (value.is_number()) {
        return Rational::approximate(value.get<double>());
    }
    if (value.is_string()) {
        return Rational::parse(value.get<std::string>());
    }
    return std::nullopt;
}

std::string encode_time(Rational value)
{
    // Never a number: a JSON number is f64 seconds, and the host model exists
    // to keep time exact, so a string carrying the fraction is the only form
    // this writer emits.
    return value.to_string();
}

nlohmann::json to_document(const Project &project, const DocumentSettings &settings)
{
    const Timeline &active = project.active();
    nlohmann::json doc = nlohmann::json::object();
    doc["concat"] = "0.1.0";
    doc["version"] = DOCUMENT_VERSION;
    doc["name"] = settings.name;
    // The active timeline's frame at the top level, the four numbers every
    // build has read there; colour stays inside the timeline itself.
    json video = json::object();
    video["width"] = active.video.width;
    video["height"] = active.video.height;
    video["rateNum"] = active.video.rate_num;
    video["rateDen"] = active.video.rate_den;
    doc["video"] = std::move(video);

    json media = json::array();
    for (const MediaItem &item : project.media) {
        media.push_back(write_media_item(item));
    }
    doc["media"] = std::move(media);
    // The flat mirror of the active timeline, for builds that predate
    // multiple timelines - the same shape Concat writes.
    json tracks = json::array();
    for (const Track &track : active.tracks) {
        tracks.push_back(write_track(track));
    }
    doc["tracks"] = std::move(tracks);
    json clips = json::array();
    for (const Clip &clip : active.clips) {
        clips.push_back(write_clip(clip));
    }
    doc["clips"] = std::move(clips);
    json fonts = json::array();
    for (const CustomFont &font : project.fonts) {
        fonts.push_back(write_font(font));
    }
    doc["fonts"] = std::move(fonts);
    json timelines = json::array();
    for (const Timeline &timeline : project.timelines) {
        timelines.push_back(write_timeline(timeline));
    }
    doc["timelines"] = std::move(timelines);
    doc["activeTimelineId"] = project.active_timeline_id;
    return doc;
}

std::optional<Project> from_document(const nlohmann::json &doc, const DocumentSettings &)
{
    // The reader ignores settings: they shape a brand-new project, not one
    // read back from a document that already carries its own timelines.
    const std::optional<json> document = migrate(doc);
    if (!document) {
        return std::nullopt;
    }
    const std::optional<Project> project = parse_project(*document);
    if (!project) {
        return std::nullopt;
    }
    return settle_project(*project);
}

} // namespace genesis::project
