// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace genesis::adapters::engine::ges {

// A strip of frames sampled across a source's span, for the timeline to draw.
// Written as a single JPEG whose tiles run left to right; returns its path or
// nullopt on failure. Blocking: callers keep it off the UI thread (R4).
struct FilmstripRequest
{
    std::filesystem::path source;
    std::filesystem::path destination;
    std::uint32_t columns = 8; // tiles across
    std::uint32_t tile_width = 160; // each tile's width; height follows aspect
    double start = 0.0; // seconds into the source
    double duration = 0.0; // 0 = to the end
};

// Renders the strip and returns its path, or nullopt when the source cannot
// be probed, has no picture or no duration, or the encode fails.
std::optional<std::filesystem::path> make_filmstrip(const FilmstripRequest &request);

// One poster frame, at `at` seconds into the source, written as a JPEG.
// Returns its path or nullopt on failure.
std::optional<std::filesystem::path> make_poster(const std::filesystem::path &source,
                                                 const std::filesystem::path &destination,
                                                 double at);

} // namespace genesis::adapters::engine::ges
