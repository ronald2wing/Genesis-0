// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Named export targets: the preset/profile catalogue an export dialog offers.
// The values are re-authored in C++ as data and pure functions; no upstream
// file is copied, and the facts themselves (1920x1080, 30 fps, "23 crf") are
// not copyrightable expression:
//
//   * encoder presets (videocodec/audiocodec) with low/med/high quality
//     hints; the encoder names are mapped onto our engine-neutral
//     ExportCodec/AudioCodec families, and the medium hint is kept as
//     `quality`.
//   * the resolution/frame-rate standards (PAL, NTSC, ATSC, DVB, DVD, BD).
//
// A target our enum cannot express - an MPEG-2/ProRes/GIF encoder, or an
// MP3/AC3/PCM audio codec - is dropped, never mapped onto the wrong family;
// the drop set is documented in ExportPresets.cpp and asserted in
// tests/render/ExportPresetsTests.cpp.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "core/ClipTimeline.h"
#include "render/ExportSpec.h"

namespace genesis::render {

// One named output standard: a frame size and its exact frame rate. The rate
// is stored as the num/den fraction ExportSpec carries, so 29.97 stays
// 30000/1001 and nothing round-trips through a double. Interlacing, SAR and
// DAR are not part of our export model (ExportSpec has no field for them), so
// a profile is resolution + rate only: "1080i" and "1080p" at one rate are
// the same entry, and a disc name ("DVD PAL") coincides with its broadcast
// twin ("PAL SD 576") because only resolution and rate distinguish them here.
struct VideoProfile
{
    std::string_view name;
    std::uint32_t width;
    std::uint32_t height;
    std::int64_t rate_num;
    std::int64_t rate_den;

    // The rate as the core FrameRate type, built exactly from num/den.
    genesis::core::FrameRate frame_rate() const
    {
        return genesis::core::FrameRate(genesis::core::Rational(rate_num, rate_den));
    }
};

// The named standard, or nullopt for a name this build does not know.
std::optional<VideoProfile> video_profile(std::string_view name);

// Every profile, in menu order.
std::span<const VideoProfile> video_profiles();

// One named export target: a codec family, a sound-codec family, and the
// frame it writes. Presets do not pin a resolution - they inherit the
// project profile - so a preset carries the host's default frame
// (1920x1080 @ 30, Timeline::hd()) and the export dialog replaces it with the
// chosen VideoProfile. The container (mp4/mkv/webm/ogg) is not part of
// ExportSpec, so targets collapse to their codec families.
struct ExportPreset
{
    std::string_view name;
    ExportCodec codec;
    AudioCodec audio_codec;
    std::uint32_t width;
    std::uint32_t height;
    std::int64_t rate_num;
    std::int64_t rate_den;
    // The medium-quality hint, as the source preset states it ("23 crf",
    // "3 Mb/s", ...). A hint for the dialog only; ExportSpec has no quality
    // field.
    std::string_view quality;
};

// The named target, or nullopt for a name this build does not know.
std::optional<ExportPreset> export_preset(std::string_view name);

// Every preset, in menu order.
std::span<const ExportPreset> export_presets();

// An ExportSpec from a preset: codec, sound codec, frame size and rate. The
// output path, the clip list and any profile override are the caller's.
ExportSpec spec_from(const ExportPreset &preset);

// The codec family name as the wire spells it ("vp8", "h264", "hevc", ...):
// the same spelling `export.run`'s `spec.codec` accepts and the engine's
// CodecPlan reports for legible errors. An enum value outside the known
// families reads "unknown".
std::string_view codec_name(ExportCodec codec);

// The first-run preset names the export dialog offers, in menu order.
std::span<const std::string_view> default_export_presets();

} // namespace genesis::render
