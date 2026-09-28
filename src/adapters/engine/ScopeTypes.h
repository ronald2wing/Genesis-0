// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace genesis::adapters::engine {

// The granularity of every scope: 256 display levels, 256 waveform columns,
// and a 256x256 vectorscope plane (the convention re-derived in Scopes.cpp).
inline constexpr std::uint32_t kScopeLevels = 256;
inline constexpr std::uint32_t kScopeColumns = 256;

// The transfer a scope frame's channel values were reduced under. Sdr means
// the bins are the raw 8-bit display code values (0..255); Hlg/Pq mean each
// channel was EOTF-decoded to a perceptual luminance scale and re-quantised to
// the same 0..255 bins, so a bin stands for a luminance level, not a code
// value. See `reduce_scopes_signal` (ges/scopes/ScopeReader.h) for the exact
// convention.
enum class ScopeSignal {
    Sdr, // Rec.709 gamma (BT.1886): bins are code values 0..255
    Hlg, // ARIB STD-B67 hybrid log-gamma: bins are nits on a 1000-nit display
    Pq, // SMPTE ST 2084 perceptual quantiser: bins are absolute nits 0..10000
};

// True when the frame's bins read on a perceptual/nit scale rather than raw
// display code values.
constexpr bool is_hdr_signal(ScopeSignal signal)
{
    return signal != ScopeSignal::Sdr;
}

// What scope bin 255 represents: the scale maximum the bins are quantised
// against. Sdr bins top out at code value 255; Pq at 10000 nits (the ST 2084
// ceiling); Hlg at 1000 nits (the HLG standard's nominal display peak).
constexpr std::uint32_t scope_scale_max(ScopeSignal signal)
{
    switch (signal) {
    case ScopeSignal::Sdr:
        return 255;
    case ScopeSignal::Hlg:
        return 1000;
    case ScopeSignal::Pq:
        return 10000;
    }
    return 255;
}

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
    // The transfer the frame was reduced under; Sdr until a frame is reduced
    // from an HDR source.
    ScopeSignal signal = ScopeSignal::Sdr;
};

} // namespace genesis::adapters::engine
