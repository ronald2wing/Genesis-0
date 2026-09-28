// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// VideoSettings model: a timeline's output frame, rate and colour.

#pragma once

#include <algorithm>
#include <cstdint>

#include "project/model/Media.h"

namespace genesis::project {

// A timeline's output frame, rate and colour.
//
// The same four numbers the document's top-level `video` block has always
// carried, now one set per timeline, and the colour it is output in. The
// top-level block is still written, as the active timeline's frame and
// rate, so a build that predates this reads the document it always did,
// and a document from such a build gives every timeline that block on the
// way in.
// On disk: camelCase.
struct VideoSettings
{
    // Output frame width in pixels.
    std::uint32_t width = 1920;
    // Output frame height in pixels.
    std::uint32_t height = 1080;
    // Numerator of the frame rate, e.g. 30000 for 29.97 fps.
    std::int64_t rate_num = 30;
    // Denominator of the frame rate, e.g. 1001 for 29.97 fps.
    std::int64_t rate_den = 1;
    // The colour the timeline is output in: SDR for every timeline from
    // before HDR, and until its first HDR clip (see
    // `Editor::follow_first_hdr`). Left out of the document for SDR, so
    // those documents stay as they were.
    ColorSpace color_space = ColorSpace::Sdr;

    // 1080p at 30 in SDR, the frame a fresh project has always been born
    // with.
    VideoSettings() = default;

    bool operator==(const VideoSettings &) const = default;

    // The rate as a number, for anything that draws or counts frames.
    double rate() const
    {
        return static_cast<double>(rate_num)
                / static_cast<double>(std::max<std::int64_t>(rate_den, 1));
    }

    // This frame with every zero or negative field taken from `fallback`
    // instead: a document hand-edited into nonsense still opens at a size
    // that is a size.
    VideoSettings or_fallback(VideoSettings fallback) const
    {
        VideoSettings out;
        out.width = width > 0 ? width : fallback.width;
        out.height = height > 0 ? height : fallback.height;
        out.rate_num = rate_num > 0 ? rate_num : fallback.rate_num;
        out.rate_den = rate_den > 0 ? rate_den : fallback.rate_den;
        out.color_space = color_space;
        return out;
    }

    // Whether every term is one a frame could actually have. A zero
    // dimension or rate is never a real setting, only a caller bug, and
    // writing one would poison the document until the next open.
    bool is_sane() const { return width > 0 && height > 0 && rate_num > 0 && rate_den > 0; }
};

} // namespace genesis::project
