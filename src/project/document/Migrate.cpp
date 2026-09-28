// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/doc.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// The document's migrations from older shapes to this build's.

#include "project/Document.h"
#include "project/document/Internal.h"
#include "project/document/Json.h"

namespace genesis::project::detail {

// A document from before multiple timelines holds one, as the top-level
// `tracks` / `clips` / `video`. Lifted into `timelines` as the one timeline it
// always was; the flat fields stay for the builds that read them.
void lift_flat_timeline(json &document)
{
    const auto timelines = document.find("timelines");
    if (timelines != document.end() && timelines->is_array() && !timelines->empty()) {
        return;
    }
    const auto tracks = document.find("tracks");
    if (tracks == document.end() || !tracks->is_array() || tracks->empty()) {
        return;
    }
    const auto clips = document.find("clips");
    json timeline = json::object();
    timeline["id"] = "TL1";
    timeline["name"] = "Timeline 1";
    timeline["tracks"] = *tracks;
    timeline["clips"] = clips != document.end() ? *clips : json::array();
    if (const auto video = document.find("video"); video != document.end()) {
        timeline["video"] = *video;
    }
    document["timelines"] = json::array({ std::move(timeline) });
}

// Timelines from before each carried its own frame take the top-level `video`,
// a field at a time: a missing or non-positive field gets the shared value.
void give_timelines_their_frame(json &document)
{
    json shared = json::object();
    if (const auto video = document.find("video"); video != document.end() && video->is_object()) {
        shared = *video;
    }
    auto timelines = document.find("timelines");
    if (timelines == document.end() || !timelines->is_array()) {
        return;
    }
    for (json &timeline : *timelines) {
        if (!timeline.is_object())
            continue;
        json video = json::object();
        if (const auto it = timeline.find("video"); it != timeline.end() && it->is_object()) {
            video = *it;
        }
        for (const char *key : { "width", "height", "rateNum", "rateDen" }) {
            const auto existing = video.find(key);
            std::int64_t n = 0;
            const bool usable = existing != video.end() && as_i64(*existing, n) && n > 0;
            if (!usable) {
                if (const auto shared_it = shared.find(key); shared_it != shared.end()) {
                    video[key] = *shared_it;
                }
            }
        }
        if (!video.empty()) {
            timeline["video"] = std::move(video);
        }
    }
}

// The document at this build's shape, or none for one from a later build.
std::optional<json> migrate(const json &doc)
{
    if (!doc.is_object()) {
        return std::nullopt;
    }
    json document = doc;
    const std::optional<int> version = document_version(document);
    if (version && *version > DOCUMENT_VERSION) {
        return std::nullopt;
    }
    lift_flat_timeline(document);
    give_timelines_their_frame(document);
    return document;
}

} // namespace genesis::project::detail
