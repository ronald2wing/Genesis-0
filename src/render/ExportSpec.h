// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "project/model/Media.h"
#include "render/ExportClip.h"

// The host's engine-free description of one export run.
//
// The host never encodes (R5): it names a codec family and the engine decides
// which element answers to it. This type stays free of GES/MLT types - the
// family names (`Vp8`, `Aac`, ...) are engine-neutral, and the element names
// (`vp8enc`, `fdkaacenc`, ...) live on the engine side of the adapter.

namespace genesis::render {

// The picture codec families the host can ask for. One entry per family the
// engine maps to a concrete encoder element.
enum class ExportCodec { Vp8, Vp9, Av1, H264, Hevc, Theora };

// The sound codec families the host can ask for. Carried even for a
// video-only timeline: the choice is the host's, the element is the engine's.
enum class AudioCodec { Vorbis, Opus, Aac, Flac };

// How a frame-rate mismatch is handled when clips whose rate differs from the
// timeline's play. Engine-independent intent, not an encoder bitrate mode.
enum class RateMode {
    // Re-time every clip onto the timeline's grid (the engine's native
    // behaviour): a 25 fps clip on a 30 fps grid is sampled, not dropped.
    Conform,
    // Drop whole frames to reach the grid rather than re-sampling.
    Drop,
    // Duplicate whole frames to reach the grid rather than re-sampling.
    Duplicate,
};

// Everything a full export needs: the destination, the output format, and the
// flattened clip list. A flat superset of the resolver's request, so the
// export flow has one type rather than a request plus a codec beside it.
struct ExportSpec
{
    // The file to write.
    std::string output;
    // Output frame width in pixels.
    std::uint32_t width = 0;
    // Output frame height in pixels.
    std::uint32_t height = 0;
    // Frame rate numerator - an exact fraction, so 29.97 stays 30000/1001.
    std::int64_t rate_num = 30;
    // Frame rate denominator; see `rate_num`.
    std::int64_t rate_den = 1;
    // The picture codec family. The engine maps it to an element.
    ExportCodec codec = ExportCodec::Vp8;
    // The sound codec family, used when the timeline has audio clips.
    AudioCodec audio_codec = AudioCodec::Opus;
    // How a rate mismatch is handled.
    RateMode rate_mode = RateMode::Conform;
    // The levels the file is written in and tagged with.
    genesis::project::ColorRange color_range = genesis::project::ColorRange::Limited;
    // What the timeline is output in.
    genesis::project::ColorSpace color_space = genesis::project::ColorSpace::Sdr;
    // An HDR timeline written as HDR rather than tone-mapped to SDR.
    bool hdr = false;
    // Ten bits a channel rather than eight. An HDR timeline always needs ten
    // bits, so `hdr` forces this on; a codec family with no ten-bit encoder is
    // a runtime refusal, not a host validation problem (R5), because only the
    // engine knows which of its elements answer a ten-bit raw format.
    bool ten_bit = false;
    // The flattened clip list to render.
    std::vector<ExportClip> clips;
};

// The problems the host can see in a spec without touching an engine: an
// empty destination, a zero-sized frame, a non-positive rate, or an empty
// timeline. Empty when the spec is sane. A codec family with no installed
// element is *not* a validation problem - it is a runtime refusal the engine
// reports, because only the engine knows its registry (R5).
std::vector<std::string> validate(const ExportSpec &spec);

} // namespace genesis::render
