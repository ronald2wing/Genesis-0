// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "project/Document.h"
#include "project/document/Internal.h"

#include <algorithm>
#include <array>

namespace genesis::project {

using nlohmann::json;
using namespace detail;

namespace {

// The top-level keys `to_document` emits. A document carrying any other
// top-level key is carrying data this build does not model, and a save will
// drop it.
bool is_modeled_top_level_key(std::string_view key)
{
    static constexpr std::array<std::string_view, 7> keys = {
        "version", "name", "video", "media", "fonts", "timelines", "activeTimelineId",
    };
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

} // namespace

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
    // A string carrying the exact fraction is the only form this reader opens.
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

std::optional<Project> from_document(const nlohmann::json &doc, const DocumentSettings &,
                                     DocumentLoadReport *report)
{
    // The reader ignores settings: they shape a brand-new project, not one
    // read back from a document that already carries its own timelines.
    if (!doc.is_object()) {
        return std::nullopt;
    }
    const std::optional<int> version = document_version(doc);
    if (version && *version > DOCUMENT_VERSION) {
        return std::nullopt; // written by a newer build, never half-loaded
    }
    if (report != nullptr) {
        report->dropped_keys.clear();
        for (const auto &[key, value] : doc.items()) {
            (void)value;
            if (!is_modeled_top_level_key(key)) {
                report->dropped_keys.push_back(key);
            }
        }
    }
    const std::optional<Project> project = parse_project(doc);
    if (!project) {
        return std::nullopt;
    }
    return settle_project(*project);
}

} // namespace genesis::project
