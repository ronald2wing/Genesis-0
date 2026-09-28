// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Media commands: importing, removing, relinking and template-slot media edits.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/ClipTimeline.h"
#include "project/model/Media.h"

namespace genesis::project {

using genesis::core::Rational;

// A media item as probed by the host, before the model mints its id.
// On disk: camelCase.
struct NewMedia
{
    // Absolute path on disk. Adding a path already in the bin is a no-op,
    // so re-imports cannot duplicate media.
    std::string path;
    // Display name for the bin, normally the file's basename.
    std::string name;
    // Seconds, or None when the container did not report one - clips of
    // such media get a five-second fallback length.
    std::optional<Rational> duration;
    // What the host's probe decided the file is.
    MediaKind kind = MediaKind::Video;
    // Pixel width, when the probe found one.
    std::optional<std::uint32_t> width;
    // Pixel height, same terms as `width`.
    std::optional<std::uint32_t> height;
    // Frames per second as a decimal, for display.
    std::optional<double> frame_rate;
    // The exact rate fraction the engine works in, e.g. "30000/1001".
    std::optional<std::string> frame_rate_fraction;
    // Codec name as probed, e.g. "h264". Informational.
    std::optional<std::string> video_codec;
    // Codec of the embedded audio, when there is any.
    std::optional<std::string> audio_codec;
    // Whether the file carries an audio stream.
    bool has_audio = false;
    // Every audio stream, in file order; see MediaItem::audio_tracks.
    // Defaulted so a caller from before the list can still add media.
    // On disk: default.
    std::vector<AudioTrack> audio_tracks;
    // What made the file, when the editor did; see MediaItem::origin.
    // Defaulted so an import, and a caller from before origins, says
    // nothing.
    // On disk: default.
    std::optional<MediaOrigin> origin;
    // What the probe found the picture recorded in; see
    // MediaItem::color_space. Defaulted, for a caller from before it,
    // to SDR.
    // On disk: default.
    ColorSpace color_space = ColorSpace::Sdr;

    bool operator==(const NewMedia &) const = default;
};

// Imports a file into the bin, minting an "m" id. A path already
// present is a tolerated no-op that mints nothing.
struct AddMedia
{
    // The probed file, as described by the host.
    NewMedia item;

    bool operator==(const AddMedia &) const = default;
};

// Removes a bin item and every clip referencing it, on *all* timelines
// - a clip whose media is gone would linger as a dead reference.
struct RemoveMedia
{
    // The bin item to remove. An unknown id is a no-op.
    std::string media_id;

    bool operator==(const RemoveMedia &) const = default;
};

// Marks a bin item as a template slot (or back to ordinary media),
// which is what Command::FillSlot requires of its target.
struct SetMediaPlaceholder
{
    // The bin item to mark. An unknown id is a no-op.
    std::string media_id;
    // True to make it a slot, false to make it ordinary media again.
    bool placeholder;

    bool operator==(const SetMediaPlaceholder &) const = default;
};

// Swaps the user's file into a template slot in place. The slot keeps
// its id so clips keep working; start, duration and speed stay the
// template's, while the in-point resets and each clip's kind and name
// follow the new file, across all timelines. Errs if the id is unknown
// or the item is not a placeholder.
struct FillSlot
{
    // The slot being filled - must have `placeholder` set.
    std::string media_id;
    // The user's file that takes the slot's place.
    NewMedia item;

    bool operator==(const FillSlot &) const = default;
};

// Registers a font file for titles. A path already registered is a
// no-op, so re-adding cannot duplicate.
struct AddFont
{
    // The family name titles will refer to.
    std::string family;
    // Where the font file lives on disk.
    std::string path;

    bool operator==(const AddFont &) const = default;
};

// Unregisters a font family. Clips keep the family name: the face may
// come back when the file does.
struct RemoveFont
{
    // The family to unregister.
    std::string family;

    bool operator==(const RemoveFont &) const = default;
};

// Updates a media item's path on disk (relink). An unknown id is a no-op.
struct UpdateMediaPath
{
    // The media item to update.
    std::string media_id;
    // The new absolute path on disk.
    std::string new_path;

    bool operator==(const UpdateMediaPath &) const = default;
};

// Says what levels a media file's picture really spans, over whatever
// the file claims: the fix for a screen recording written full range
// and tagged nothing, which plays grey where it should be black, or a
// video-range file tagged full, which crushes its shadows. Reaches
// every clip of the media, on every timeline, in the monitor and the
// export alike. An unknown id is a no-op.
struct SetMediaColorRange
{
    // The bin item.
    std::string media_id;
    // `limited` or `full`, or None to go back to reading the file's
    // own tag.
    std::optional<ColorRange> range;

    bool operator==(const SetMediaColorRange &) const = default;
};

} // namespace genesis::project
