// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Media model: kinds, origins, colour, the probed media item and missing media.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/ClipTimeline.h"

namespace genesis::project {

using genesis::core::Rational;

// What a piece of media is. A still is not a video with one frame: it has no
// intrinsic duration, so its length on a timeline is editorial.
// On disk: lowercase.
enum class MediaKind {
    // Moving pictures, with or without embedded sound.
    Video,
    // Sound only; nothing to composite.
    Audio,
    // A still, whose timeline length is editorial rather than intrinsic.
    Image,
};

// What made a media file, when it was the editor and not the user's
// import. The bin shelves such a file under Generated, by origin, and
// keeps it out of the import shelves.
// On disk: lowercase.
enum class MediaOrigin {
    // Read aloud by the speech sheet.
    Speech,
    // A clip's sound rendered as it played - trimmed, at its speed, with
    // its level, fades and effects baked in - as a file of its own.
    Processed,
    // A clip's picture upscaled (and denoised) by the enhance runtime -
    // the Real-ESRGAN copy that replaces the clip's source frame for frame.
    Enhanced,
};

// One audio stream of a media file, as the probe reported it. A screen
// recording keeps the desktop's sound and the microphone as two of these;
// a clip names the one it plays by index.
// On disk: camelCase.
struct AudioTrack
{
    // The stream's index within the file - what Clip::audio_stream holds.
    std::uint32_t index = 0;
    // Codec short name, e.g. "aac". Informational.
    std::string codec;
    // Channel count: 1 mono, 2 stereo.
    std::uint32_t channels = 0;
    // Samples per second.
    std::uint32_t sample_rate = 0;
    // The name the file gives the track, when it gives one ("Desktop
    // Audio", "Mic/Aux"); empty otherwise, and the UI numbers it.
    std::string title;
    // The track's language tag, e.g. "eng"; empty when unstated.
    std::string language;

    bool operator==(const AudioTrack &) const = default;
};

// The levels a media file's picture spans, when the person says so over
// whatever the file claims: video range, 16-235, or full range, 0-255.
// A screen recording written full and tagged nothing plays grey where it
// should be black until it is read as full; a video-range file tagged
// full crushes its shadows until it is read as limited. Set with
// Command::SetMediaColorRange; absent means the file's own tag is read.
// https://github.com/jub0t/Concat/issues/103
// On disk: lowercase.
enum class ColorRange {
    // 16-235: what broadcast, cameras and every player expect.
    Limited,
    // 0-255: what screen recorders and some phones write.
    Full,
};

// The colour a picture is in: SDR on Rec. 709, or HDR on Rec. 2020 with
// the hybrid log-gamma or the perceptual quantiser transfer. A timeline's
// is what it is output in; a media file's, what it was recorded in.
// On disk: lowercase.
enum class ColorSpace {
    // Rec. 709, SDR: every timeline until its first HDR clip.
    Sdr,
    // Rec. 2020 with HLG: what an iPhone records, and what a timeline
    // becomes on its first HDR clip.
    Hlg,
    // Rec. 2020 with PQ: HDR10.
    Pq,
};

// Standard dynamic range: what a document that says nothing is in.
constexpr bool is_sdr(ColorSpace space)
{
    return space == ColorSpace::Sdr;
}

// High dynamic range, HLG or PQ.
constexpr bool is_hdr(ColorSpace space)
{
    return !is_sdr(space);
}

// One entry in the media bin: a file the user imported, plus what the host's
// probe learned about it. The probe metadata is stored, not re-derived, so a
// document opens meaningfully even when the file itself is missing.
// On disk: camelCase.
struct MediaItem
{
    // Minted by the editor ("m1", "m2", ...) and never re-issued, even
    // across a save/load - clips reference media by this id.
    std::string id;
    // Absolute path on the user's disk. Doubles as the duplicate check:
    // adding the same path twice is a no-op.
    std::string path;
    // Display name in the bin, normally the file's basename.
    std::string name;
    // Seconds. None when the container did not say.
    std::optional<Rational> duration;
    // What the probe decided the file is; fixes which ClipKind its
    // clips get.
    MediaKind kind = MediaKind::Video;
    // Pixel width, when the file has pictures and the probe found one.
    std::optional<std::uint32_t> width;
    // Pixel height, same terms as `width`.
    std::optional<std::uint32_t> height;
    // Frames per second as a decimal - convenient for display, not exact.
    std::optional<double> frame_rate;
    // The exact fraction the engine works in, e.g. "30000/1001".
    std::optional<std::string> frame_rate_fraction;
    // Codec name as the probe reported it, e.g. "h264". Informational.
    std::optional<std::string> video_codec;
    // Codec of the embedded audio, when there is any.
    std::optional<std::string> audio_codec;
    // Whether the file carries an audio stream; gates DetachAudio.
    bool has_audio = false;
    // Every audio stream the file carries, in file order, when the probe
    // listed them. Empty for a file without sound and for a document from
    // before the list was kept - `has_audio` still says whether there is
    // sound at all. More than one is a recording with its tracks apart, and
    // what lets a clip choose between them. Skipped when empty, so
    // documents without such media stay byte-identical.
    std::vector<AudioTrack> audio_tracks;
    // True when this item is a template slot: a stand-in whose metadata says
    // what kind of media belongs here, waiting to be replaced by the user's
    // own file (`Command::FillSlot`). In a creator's own project the path is
    // still real; only a packed template bundle blanks it. Skipped when
    // false, so documents without templates stay byte-identical.
    bool placeholder = false;
    // The levels the picture is read as, over the file's own tag; see
    // ColorRange. Absent, and left out of the document, for the tag.
    std::optional<ColorRange> color_range;
    // What the probe found the picture recorded in: HLG or PQ for HDR.
    // Left out of the document for SDR - and absent from one written
    // before it was kept, which reads as SDR.
    ColorSpace color_space = ColorSpace::Sdr;
    // Where the file came from when the editor made it; see
    // MediaOrigin. Absent, and left out of the document, for an
    // import, and read as absent when it names an origin this build does
    // not know - the file is still a file, just one shelved with the
    // imports.
    std::optional<MediaOrigin> origin;

    // A nameless entry is called by its path.
    MediaItem tidy() const
    {
        MediaItem out = *this;
        if (out.name.empty()) {
            out.name = out.path;
        }
        return out;
    }

    // Which row of `audio_tracks` a clip's `audio_stream` is: the named
    // stream's position, or the first row for a clip that names none or
    // names a stream this file does not have - the same fallback the
    // engine's readers make.
    std::size_t audio_track_position(std::optional<std::uint32_t> stream) const
    {
        if (!stream) {
            return 0;
        }
        const auto it =
                std::find_if(audio_tracks.begin(), audio_tracks.end(),
                             [stream](const AudioTrack &track) { return track.index == *stream; });
        if (it == audio_tracks.end()) {
            return 0;
        }
        return static_cast<std::size_t>(it - audio_tracks.begin());
    }

    bool operator==(const MediaItem &) const = default;
};

// A media reference that points to a non-existent file.
struct MissingMedia
{
    // The media item's stable id (e.g. "m1", "m2").
    std::string id;
    // Display name in the bin.
    std::string name;
    // The absolute path that no longer exists on disk.
    std::string path;
};

} // namespace genesis::project
