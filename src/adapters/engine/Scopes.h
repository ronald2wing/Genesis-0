// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace genesis::adapters::engine {

// The granularity of every scope: 256 display levels, 256 waveform columns,
// and a 256x256 vectorscope plane (the Concat convention, re-derived in
// Scopes.cpp).
inline constexpr std::uint32_t kScopeLevels = 256;
inline constexpr std::uint32_t kScopeColumns = 256;

// One 256-bin histogram, four channels: red, green, blue, and Rec.709 luma.
// Each bin counts the pixels whose channel value (0..255) falls in that bin.
struct Histogram
{
    std::array<std::uint32_t, kScopeLevels> red{ };
    std::array<std::uint32_t, kScopeLevels> green{ };
    std::array<std::uint32_t, kScopeLevels> blue{ };
    std::array<std::uint32_t, kScopeLevels> luma{ };
};

// The waveform scope: per-column min/max display value (0..255) for each
// channel. `columns` is 0 until a frame has been reduced.
struct Waveform
{
    std::uint32_t columns = 0;
    std::array<std::uint8_t, kScopeColumns> red_min{ };
    std::array<std::uint8_t, kScopeColumns> red_max{ };
    std::array<std::uint8_t, kScopeColumns> green_min{ };
    std::array<std::uint8_t, kScopeColumns> green_max{ };
    std::array<std::uint8_t, kScopeColumns> blue_min{ };
    std::array<std::uint8_t, kScopeColumns> blue_max{ };
    std::array<std::uint8_t, kScopeColumns> luma_min{ };
    std::array<std::uint8_t, kScopeColumns> luma_max{ };
};

// The vectorscope: a density plot over the Cb/Cr colour plane. `size` is 0
// until a frame has been reduced; `density` is size*size, row-major, row 0 at
// the top (the +Cr half-plane that red saturates into).
struct Vectorscope
{
    std::uint32_t size = 0;
    std::vector<std::uint32_t> density;
};

// All three scopes of one frame.
struct ScopeFrame
{
    Histogram histogram;
    Waveform waveform;
    Vectorscope vectorscope;
};

} // namespace genesis::adapters::engine
