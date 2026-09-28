// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "project/Project.h"
#include "project/model/Text.h"

// The title-picture requests a host hands to the app: every title clip in the
// project that needs a picture, in timeline order, together with the words, the
// style, and the canvas the app must rasterise against.

namespace genesis::render {

// A title clip needs a picture before the engine can show it. The host knows
// the words and the style; it cannot draw them - the app can (Qt is there and
// the host must not grow a font engine). This is that request.
struct TitleRequest
{
    std::string clip_id;
    std::string text;
    genesis::project::TextStyle style;
    std::uint32_t canvas_width = 0;
    std::uint32_t canvas_height = 0;
    bool operator==(const TitleRequest &) const = default;
};

// Every clip in the project that is a title and needs a picture, in timeline
// order. A clip whose text is empty yields no request (nothing to draw).
std::vector<TitleRequest> title_requests(const genesis::project::Project &project);

} // namespace genesis::render
