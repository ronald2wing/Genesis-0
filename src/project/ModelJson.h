// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The model fragments the document writer and the command codec both emit. The
// two schemas are separate, but these values have the same shape in both; one
// writer keeps a key from being renamed on one side only.

#pragma once

#include <nlohmann/json.hpp>

#include "project/model/Cutout.h"
#include "project/model/Media.h"

namespace genesis::project::detail {

using nlohmann::json;

// The audio-track fragment: the identity fields plus the optional title and
// language, which are omitted when empty.
inline json audio_track_json(const AudioTrack &track)
{
    json j = json::object();
    j["index"] = track.index;
    j["codec"] = track.codec;
    j["channels"] = track.channels;
    j["sampleRate"] = track.sample_rate;
    if (!track.title.empty()) {
        j["title"] = track.title;
    }
    if (!track.language.empty()) {
        j["language"] = track.language;
    }
    return j;
}

inline json crop_json(const Crop &crop)
{
    json j = json::object();
    j["left"] = crop.left;
    j["top"] = crop.top;
    j["right"] = crop.right;
    j["bottom"] = crop.bottom;
    return j;
}

} // namespace genesis::project::detail
