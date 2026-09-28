// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// See ExportPresets.h for provenance: the values here are re-authored from
// OpenShot openshot-qt (GPL-3.0-or-later), 2026-09-30, under this work's
// GPL-3.0-or-later license.

#include "render/ExportPresets.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace genesis::render {

namespace {

// ---------------------------------------------------------------------------
// Video profiles
// ---------------------------------------------------------------------------
//
// One entry per (resolution, rate) standard. The rate is the exact fraction
// ExportSpec uses: 23.976 is 24000/1001, 29.97 is 30000/1001 and 59.94 is
// 60000/1001. Every rate OpenShot's definitions/ catalogue names is exactly
// representable this way (denominators 1 or 1001), so none is skipped.
//
// "29.97 drop-frame" is not a rate we have to drop: drop-frame is a timecode
// *display* convention (frame numbers 00 and 01 are skipped each minute except
// the tenth), while the underlying rate is exactly 30000/1001 - the same value
// here as the non-drop-frame 29.97. Our Rational holds that exactly, so the
// entry is kept rather than flagged.
//
// Interlacing, SAR and DAR are not part of our export model (ExportSpec has
// no field for them), so a profile is resolution + rate only. "1080i" and
// "1080p" at one rate collapse to one entry, and the disc names ("DVD PAL")
// coincide with their broadcast twins ("PAL SD 576") for the same reason.

inline constexpr std::array<VideoProfile, 32> kVideoProfiles = {
    // 720p (1280x720).
    VideoProfile{ "720p 23.976", 1280, 720, 24000, 1001 },
    VideoProfile{ "720p 24", 1280, 720, 24, 1 },
    VideoProfile{ "720p 25", 1280, 720, 25, 1 },
    VideoProfile{ "720p 29.97", 1280, 720, 30000, 1001 },
    VideoProfile{ "720p 30", 1280, 720, 30, 1 },
    VideoProfile{ "720p 50", 1280, 720, 50, 1 },
    VideoProfile{ "720p 59.94", 1280, 720, 60000, 1001 },
    VideoProfile{ "720p 60", 1280, 720, 60, 1 },
    // 1080p (1920x1080).
    VideoProfile{ "1080p 23.976", 1920, 1080, 24000, 1001 },
    VideoProfile{ "1080p 24", 1920, 1080, 24, 1 },
    VideoProfile{ "1080p 25", 1920, 1080, 25, 1 },
    VideoProfile{ "1080p 29.97", 1920, 1080, 30000, 1001 },
    VideoProfile{ "1080p 30", 1920, 1080, 30, 1 },
    VideoProfile{ "1080p 50", 1920, 1080, 50, 1 },
    VideoProfile{ "1080p 59.94", 1920, 1080, 60000, 1001 },
    VideoProfile{ "1080p 60", 1920, 1080, 60, 1 },
    // 4K UHD (3840x2160).
    VideoProfile{ "2160p 23.976", 3840, 2160, 24000, 1001 },
    VideoProfile{ "2160p 24", 3840, 2160, 24, 1 },
    VideoProfile{ "2160p 25", 3840, 2160, 25, 1 },
    VideoProfile{ "2160p 29.97", 3840, 2160, 30000, 1001 },
    VideoProfile{ "2160p 30", 3840, 2160, 30, 1 },
    VideoProfile{ "2160p 50", 3840, 2160, 50, 1 },
    VideoProfile{ "2160p 59.94", 3840, 2160, 60000, 1001 },
    VideoProfile{ "2160p 60", 3840, 2160, 60, 1 },
    // PAL / NTSC standard definition, and their disc twins.
    VideoProfile{ "PAL SD 576", 720, 576, 25, 1 },
    VideoProfile{ "NTSC SD 480", 720, 480, 30000, 1001 },
    VideoProfile{ "DVD PAL", 720, 576, 25, 1 },
    VideoProfile{ "DVD NTSC", 720, 480, 30000, 1001 },
    // ATSC broadcast modes (1080 and 720p).
    VideoProfile{ "ATSC 720p 59.94", 1280, 720, 60000, 1001 },
    VideoProfile{ "ATSC 1080 29.97", 1920, 1080, 30000, 1001 },
    // Blu-ray.
    VideoProfile{ "BD 1080p 24", 1920, 1080, 24, 1 },
    VideoProfile{ "BD 2160p 59.94", 3840, 2160, 60000, 1001 },
};

// ---------------------------------------------------------------------------
// Export presets
// ---------------------------------------------------------------------------
//
// One entry per (codec family, sound-codec family) target. OpenShot's encoder
// names map onto our families as: libx264 / h264_dxva2 / h264_vaapi /
// h264_nvenc / h264_qsv / h264_videotoolbox -> H264; libvpx -> Vp8;
// libvpx-vp9 / vp9_vaapi -> Vp9; libsvtav1 / librav1e -> Av1; libtheora ->
// Theora; libx265 / hevc_vaapi -> Hevc. Sound: aac -> Aac, libvorbis ->
// Vorbis, flac -> Flac, libopus -> Opus.
//
// The hardware-accelerated encoder variants (dxva2/vaapi/nvenc/qsv/
// videotoolbox) and the second AV1 encoder (librav1e) collapse into their
// family's software representative: our family model names a codec family,
// not which element answers to it, so "h264_nvenc" and "libx264" are both
// H264 here. The lossless x264/vp9 variants are collapsed too - "lossless" is
// a quality mode ExportSpec cannot express. The webm vp9_vaapi + libopus
// target folds into "vp9"; Opus is expressible but its only source preset is
// that hardware variant, so no included preset maps to it.
//
// Dropped, never mapped onto the wrong family (video codec has no enum value):
//   mpeg2video                -> MPEG-2 (format_mpeg_mpeg2, format_mov_mpeg2,
//                               format_avi_mpeg2, dvd_ntsc, dvd_pal)
//   mpeg4                     -> MPEG-4 Part 2 (format_mp4_mpeg4,
//                               format_avi_mp4, format_mov_mpeg4, metacafe)
//   libxvid                   -> Xvid (format_mp4_xvid, nokia_nHD, xbox360)
//   prores_ks                 -> ProRes (format_mov_prores)
//   gif                       -> GIF (format_gif)
// Dropped (audio codec has no enum value):
//   libmp3lame -> MP3 (format_mp3, format_avi_mp4, format_mov_mpeg4,
//                format_mp4_mpeg4); ac3 (dvd_ntsc, dvd_pal); mp2
//                (format_avi_mpeg2, format_mov_mpeg2, format_mpeg_mpeg2);
//                pcm_s16le (format_mov_prores).
//
// OpenShot presets do not pin a resolution, so each carries the host's
// default frame (1920x1080 @ 30, Timeline::hd()); the dialog overrides it
// with the selected VideoProfile. The container (mp4/mkv/webm/ogg/mov/avi) is
// not part of ExportSpec, so "MP4 (h.264)" and "MKV (h.264)" are one target.

inline constexpr std::array<ExportPreset, 8> kExportPresets = {
    // The dominant target: H.264 + AAC, from MP4 (h.264) and the MKV/MOV/AVI
    // x264 and hardware x264 variants.
    ExportPreset{ "h264", ExportCodec::H264, AudioCodec::Aac, 1920, 1080, 30, 1, "23 crf" },
    // YouTube's recommended H.264 upload bitrate.
    ExportPreset{ "youtube", ExportCodec::H264, AudioCodec::Aac, 1920, 1080, 30, 1, "10 Mb/s" },
    // WEBM (vpx).
    ExportPreset{ "vp8", ExportCodec::Vp8, AudioCodec::Vorbis, 1920, 1080, 30, 1, "3 Mb/s" },
    // WEBM (vp9).
    ExportPreset{ "vp9", ExportCodec::Vp9, AudioCodec::Vorbis, 1920, 1080, 30, 1, "34 crf" },
    // MP4 (AV1 svt).
    ExportPreset{ "av1", ExportCodec::Av1, AudioCodec::Aac, 1920, 1080, 30, 1, "38 qp" },
    // MP4 (H.265) from libx265 and the hardware HEVC variants.
    ExportPreset{ "hevc", ExportCodec::Hevc, AudioCodec::Aac, 1920, 1080, 30, 1, "23 crf" },
    // OGG (theora/vorbis).
    ExportPreset{ "theora", ExportCodec::Theora, AudioCodec::Vorbis, 1920, 1080, 30, 1, "3 Mb/s" },
    // OGG (theora/flac).
    ExportPreset{ "theora_flac", ExportCodec::Theora, AudioCodec::Flac, 1920, 1080, 30, 1,
                  "3 Mb/s" },
};

// The first-run preset names the export dialog offers.
inline constexpr std::array<std::string_view, 4> kDefaultExportPresets = {
    "h264",
    "vp9",
    "av1",
    "theora",
};

} // namespace

std::optional<VideoProfile> video_profile(std::string_view name)
{
    for (const VideoProfile &profile : kVideoProfiles) {
        if (profile.name == name) {
            return profile;
        }
    }
    return std::nullopt;
}

std::span<const VideoProfile> video_profiles()
{
    return kVideoProfiles;
}

std::optional<ExportPreset> export_preset(std::string_view name)
{
    for (const ExportPreset &preset : kExportPresets) {
        if (preset.name == name) {
            return preset;
        }
    }
    return std::nullopt;
}

std::span<const ExportPreset> export_presets()
{
    return kExportPresets;
}

ExportSpec spec_from(const ExportPreset &preset)
{
    ExportSpec spec;
    spec.codec = preset.codec;
    spec.audio_codec = preset.audio_codec;
    spec.width = preset.width;
    spec.height = preset.height;
    spec.rate_num = preset.rate_num;
    spec.rate_den = preset.rate_den;
    return spec;
}

std::span<const std::string_view> default_export_presets()
{
    return kDefaultExportPresets;
}

} // namespace genesis::render
