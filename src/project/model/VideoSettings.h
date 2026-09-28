// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// VideoSettings model: a timeline's output frame, rate and colour.

#pragma once

#include <algorithm>
#include <cstdint>

#include "project/model/Media.h"

namespace genesis::project {

// The editor's output-frame bounds. A side below 16 px or above 8192 px, or
// a rate outside 1..240 fps, is not an output the editor produces: the
// command layer refuses one, and the document settle pass repairs it to the
// default. Bounds live here so both paths read the same numbers.
inline constexpr std::uint32_t kMinVideoSide = 16;
inline constexpr std::uint32_t kMaxVideoSide = 8192;
inline constexpr double kMinVideoFps = 1.0;
inline constexpr double kMaxVideoFps = 240.0;

// Whether a side is one an output frame can have.
inline bool side_in_range(std::uint32_t side)
{
    return side >= kMinVideoSide && side <= kMaxVideoSide;
}

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

    // This frame with every field outside the output bounds taken from
    // `fallback` instead: a document hand-edited into nonsense still opens
    // at a size and rate that are real.
    VideoSettings or_fallback(VideoSettings fallback) const
    {
        VideoSettings out;
        out.width = side_in_range(width) ? width : fallback.width;
        out.height = side_in_range(height) ? height : fallback.height;
        const bool rate_ok = rate_in_range();
        out.rate_num = rate_ok ? rate_num : fallback.rate_num;
        out.rate_den = rate_ok ? rate_den : fallback.rate_den;
        out.color_space = color_space;
        return out;
    }

    // Whether the rate is a positive pair inside the fps bounds.
    bool rate_in_range() const
    {
        return rate_num > 0 && rate_den > 0 && rate() >= kMinVideoFps && rate() <= kMaxVideoFps;
    }
};

} // namespace genesis::project
