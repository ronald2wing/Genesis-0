// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "../support/Checks.h"
#include "core/ClipTimeline.h"
#include "render/ExportClip.h"
#include "render/ExportPresets.h"
#include "render/ExportSpec.h"

namespace {

using genesis::test::check;
namespace render = genesis::render;
using genesis::core::FrameRate;
using genesis::core::Rational;

// The presets this build drops because our enum cannot express them.
// They are asserted absent (never present-as-something-else) below:
//
//   video codec has no ExportCodec value:
//     dvd_ntsc, dvd_pal            -> mpeg2video + ac3
//     format_avi_mpeg2             -> mpeg2video + mp2
//     format_mov_mpeg2             -> mpeg2video + mp2
//     format_mpeg_mpeg2            -> mpeg2video + mp2
//     format_mp4_mpeg4             -> mpeg4 + libmp3lame
//     format_avi_mp4               -> mpeg4 + libmp3lame
//     format_mov_mpeg4             -> mpeg4 + libmp3lame
//     metacafe                     -> mpeg4
//     format_mp4_xvid              -> libxvid
//     nokia_nHD                    -> libxvid
//     xbox360                      -> libxvid
//     format_mov_prores            -> prores_ks + pcm_s16le
//     format_gif                   -> gif
//   audio-only with no AudioCodec value:
//     format_mp3                   -> libmp3lame (MP3)
//
// The HEVC targets (format_mkv_x265, format_mp4_x265_crf, format_mp4_hevc_hw)
// are NOT in this set: they map onto our Hevc family and are asserted present
// in test_representative_hevc_preset_field_by_field below.
constexpr const char *kExcludedPresets[] = {
    "dvd_ntsc",          "dvd_pal",          "format_avi_mpeg2", "format_mov_mpeg2",
    "format_mpeg_mpeg2", "format_mp4_mpeg4", "format_avi_mp4",   "format_mov_mpeg4",
    "metacafe",          "format_mp4_xvid",  "nokia_nHD",        "xbox360",
    "format_mov_prores", "format_gif",       "format_mp3",
};

// A spec ready for validate(): one preset applied, plus the caller-owned
// output path and a single blank video clip.
render::ExportSpec complete_spec(const render::ExportPreset &preset)
{
    render::ExportSpec spec = render::spec_from(preset);
    spec.output = "/tmp/export_presets_test.mp4";
    spec.clips.push_back(
            render::ExportClip::blank(render::ClipKind::Video, Rational::ZERO, Rational::ONE, 0));
    return spec;
}

void test_every_profile_rate_round_trips_exactly()
{
    const std::span<const render::VideoProfile> profiles = render::video_profiles();
    check(!profiles.empty(), "some video profiles exist");

    for (const render::VideoProfile &profile : profiles) {
        // The rate, rebuilt through our exact rational type, is exactly the
        // stored fraction - reduced and identical, never an approximation.
        const FrameRate rebuilt(profile.frame_rate());
        check(rebuilt.fps() == Rational(profile.rate_num, profile.rate_den),
              "profile " + std::string(profile.name)
                      + " rate round-trips through Rational exactly");
        check(rebuilt.fps().numerator() == profile.rate_num
                      && rebuilt.fps().denominator() == profile.rate_den,
              "profile " + std::string(profile.name) + " rate is already in lowest terms");
        check(profile.rate_num > 0 && profile.rate_den > 0,
              "profile " + std::string(profile.name) + " rate is positive");
    }
}

void test_every_preset_produces_a_valid_spec()
{
    const std::span<const render::ExportPreset> presets = render::export_presets();
    check(!presets.empty(), "some export presets exist");

    for (const render::ExportPreset &preset : presets) {
        const render::ExportSpec spec = complete_spec(preset);
        const std::vector<std::string> problems = render::validate(spec);
        check(problems.empty(),
              "preset " + std::string(preset.name) + " produces a spec that validates: "
                      + (problems.empty() ? std::string() : problems.front()));
    }
}

void test_representative_h264_preset_field_by_field()
{
    const std::optional<render::ExportPreset> preset = render::export_preset("h264");
    check(preset.has_value(), "h264 preset resolves");
    check(preset->codec == render::ExportCodec::H264, "h264 maps to the H264 family");
    check(preset->audio_codec == render::AudioCodec::Aac, "h264 maps to Aac audio");
    check(preset->width == 1920 && preset->height == 1080, "h264 targets 1920x1080");
    check(preset->rate_num == 30 && preset->rate_den == 1, "h264 targets 30 fps");
    check(preset->quality == "23 crf", "h264 medium-quality hint is 23 crf");
}

void test_representative_hevc_preset_field_by_field()
{
    const std::optional<render::ExportPreset> preset = render::export_preset("hevc");
    check(preset.has_value(), "hevc preset resolves");
    check(preset->codec == render::ExportCodec::Hevc, "hevc maps to the Hevc family");
    check(preset->audio_codec == render::AudioCodec::Aac, "hevc maps to Aac audio");
    check(preset->width == 1920 && preset->height == 1080, "hevc targets 1920x1080");
    check(preset->rate_num == 30 && preset->rate_den == 1, "hevc targets 30 fps");

    // The preset produces a host-valid spec, ten-bit and HDR defaulting off -
    // the export dialog opts into depth, not the preset.
    render::ExportSpec spec = render::spec_from(*preset);
    check(spec.codec == render::ExportCodec::Hevc, "hevc spec_from carries the Hevc family");
    check(!spec.ten_bit, "a preset defaults to eight bits");
    check(!spec.hdr, "a preset defaults to SDR");
}

void test_excluded_targets_are_absent_not_remapped()
{
    for (const char *name : kExcludedPresets) {
        check(!render::export_preset(name).has_value(),
              "excluded preset " + std::string(name)
                      + " is absent, not mapped onto the wrong family");
    }
}

void test_lookup_and_defaults()
{
    check(render::video_profile("1080p 30").has_value(), "a known video profile is present");
    check(!render::video_profile("no.such.profile").has_value(),
          "an unknown video profile is nullopt, not a default");

    check(render::export_preset("vp9").has_value(), "a known export preset is present");
    check(!render::export_preset("no.such.preset").has_value(),
          "an unknown export preset is nullopt, not a default");

    const std::span<const std::string_view> defaults = render::default_export_presets();
    check(!defaults.empty(), "some first-run defaults exist");
    for (const std::string_view name : defaults) {
        check(render::export_preset(name).has_value(),
              "first-run default " + std::string(name) + " resolves to a preset");
    }
}

void test_codec_name_maps_every_family()
{
    check(render::codec_name(render::ExportCodec::Vp8) == "vp8", "Vp8 spells vp8");
    check(render::codec_name(render::ExportCodec::Vp9) == "vp9", "Vp9 spells vp9");
    check(render::codec_name(render::ExportCodec::Av1) == "av1", "Av1 spells av1");
    check(render::codec_name(render::ExportCodec::H264) == "h264", "H264 spells h264");
    check(render::codec_name(render::ExportCodec::Hevc) == "hevc", "Hevc spells hevc");
    check(render::codec_name(render::ExportCodec::Theora) == "theora", "Theora spells theora");

    // Every preset's codec family spells a non-empty name - the wire never
    // reports a family it cannot name.
    for (const render::ExportPreset &preset : render::export_presets()) {
        check(!render::codec_name(preset.codec).empty(),
              "preset " + std::string(preset.name) + " has a non-empty codec name");
    }
}

} // namespace

int main()
{
    test_every_profile_rate_round_trips_exactly();
    test_every_preset_produces_a_valid_spec();
    test_representative_h264_preset_field_by_field();
    test_representative_hevc_preset_field_by_field();
    test_excluded_targets_are_absent_not_remapped();
    test_lookup_and_defaults();
    test_codec_name_maps_every_family();

    return genesis::test::summary();
}
