// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-export/src/lib.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ai/vision/MaskStore.h"
#include "core/Animation.h"
#include "core/ClipTimeline.h"
#include "project/model/Clip.h"
#include "project/model/Cutout.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"

namespace genesis::render {

// The flattened, engine-ready clip list a host hands an engine: one clip per
// type, its timing, mix, transform and keys. This is the seam where a
// flattened clip list becomes the engine's timeline, and from there the
// engine decides everything. These types are the host graph - engine-free by
// rule: no GES/MLT type appears here.

// What a flattened clip is. Typed, so a kind check the compiler has not
// seen cannot exist - the document says "video"/"audio"/"image".
enum class ClipKind {
    // Footage: pictures over time, possibly with its own sound.
    Video,
    // Sound only. Contributes to the mix and never to the picture.
    Audio,
    // A still: a one-frame stream, decoded looping.
    Image,
    // A treatment with no pixels of its own: its chain runs over everything
    // composited beneath its track for its span, blended back by its
    // opacity, ramped by its fades. See `composite_treated`.
    Layer,
};

// True for the kinds that put pixels on screen.
constexpr bool is_visual(ClipKind kind)
{
    return kind == ClipKind::Video || kind == ClipKind::Image;
}

// One animation key, as the flattener hands it over.
struct ExportKey
{
    // "scale", "offsetX", "offsetY", "rotation", "opacity" or "volume".
    genesis::project::KeyProperty property = genesis::project::KeyProperty::Scale;
    // Where in the clip, `0..=1`.
    double at = 0.0;
    // The value there. Relative to the clip's own for a key that came
    // from an animation preset, and absolute for one the user set - which
    // is the same thing, because `flatten::export_base` hands the engine a
    // neutral constant for every property the user has keyed.
    double value = 0.0;
    // The timing function into this key, as a CSS cubic-bezier's two
    // control points: `[x1, y1, x2, y2]`. Absent is a straight line.
    genesis::core::Ease ease = genesis::core::Ease::linear();

    bool operator==(const ExportKey &) const = default;
};

// A transition on the cut into a clip.
struct TransitionSpec
{
    // "cross-fade", "fade-black", "fade-white", "push", "zoom", "wipe-left"
    // or "wipe-right". Anything else renders as a cut.
    std::string kind;
    // Seconds the transition covers.
    genesis::core::Rational duration = genesis::core::Rational::ONE;

    bool operator==(const TransitionSpec &) const = default;
};

// One clip, as the frontend's flattener describes it.
struct ExportClip
{
    // The media file this clip shows or plays.
    std::string path;
    // Which of the file's audio streams the clip plays, by stream index;
    // absent is the first in file order. See the document's
    // `Clip::audio_stream`.
    std::optional<std::uint32_t> audio_stream;
    // The levels the file is read as, over its own tag; absent reads the
    // tag. See the document's `MediaItem::color_range`.
    std::optional<genesis::project::ColorRange> color_range;
    // Whether the clip is footage, sound or a still.
    ClipKind kind = ClipKind::Video;
    // Seconds into the timeline where the clip begins.
    genesis::core::Rational start = genesis::core::Rational::ZERO;
    // Seconds of timeline the clip covers.
    genesis::core::Rational duration = genesis::core::Rational::ONE;
    // Seconds into the source where playback begins.
    genesis::core::Rational source_start = genesis::core::Rational::ZERO;
    // Index into the track stack, zero being bottom-most.
    std::size_t track = 0;
    // The track's picture is switched off.
    bool hidden = false;
    // The track's sound is switched off.
    bool muted = false;
    // Linear gain, 1 being unity.
    double volume = 1.0;
    // Stereo pan, -1 full left, +1 full right; 0 centred. A constant, not a
    // keyable ride.
    double pan = 0.0;
    // Audio fade up from the clip's start, in seconds.
    genesis::core::Rational fade_in = genesis::core::Rational::ZERO;
    // Audio fade out into the clip's end, in seconds.
    genesis::core::Rational fade_out = genesis::core::Rational::ZERO;
    // FFmpeg filter chain from the Filters tab, or empty.
    std::string filter_chain;
    // Playback rate. 1 is normal.
    double speed = 1.0;
    // False lets pitch rise with the rate, like tape.
    bool preserve_pitch = true;
    // Speed over the clip as `(at, speed)` points, `at` a fraction of the
    // clip's length; empty for the constant `speed`. See `SpeedCurve`.
    std::vector<std::pair<double, double>> speed_curve;
    // Keys over the clip's placement and opacity, resolved by the UI from
    // Empty for none.
    std::vector<ExportKey> animation;
    // Mirrored left to right.
    bool flip_h = false;
    // Mirrored top to bottom.
    bool flip_v = false;
    // Played backwards: the covered source span shows in reverse. Carried by
    // the flattener so the app can generate a reversed copy of the source
    // span before the timeline is built; the engine itself never reverses.
    bool reverse = false;
    // The blend mode's name; empty or "normal" is source-over.
    std::string blend;
    // Fractions cut off the source's left, top, right and bottom before it
    // is fitted; absent for none.
    std::optional<std::array<double, 4>> crop;
    // The clip's applied effects, as the document holds them: each one's
    // shader runs on the GPU. A link no package answers to draws nothing.
    std::vector<genesis::project::AppliedFilter> effects;
    // The clip's audio effects, as the document holds them (`Clip::filters`):
    // each one's backend runs on the clip's audio path. The audio-path
    // sibling of `effects`, carried separately so the two never share a
    // pack's kind.
    std::vector<genesis::project::AppliedFilter> audio_filters;
    // Multiplier over the fitted size. 1 fills the frame, preserving aspect.
    double scale = 1.0;
    // Offset of the picture's centre from frame centre, frame-width fraction.
    double offset_x = 0.0;
    // Offset as a frame-height fraction.
    double offset_y = 0.0;
    // Clockwise rotation in degrees.
    double rotation = 0.0;
    // Multipliers on the fitted width and height beyond `scale`, for a
    // picture pulled along one axis; 1 keeps the aspect.
    double stretch_x = 1.0;
    // The height's half of `stretch_x`'s pair.
    double stretch_y = 1.0;
    // Blend strength over the layers beneath, 1 being solid. Defaulted for
    // requests from a UI that predates it.
    double opacity = 1.0;
    // The transition into this clip's cut, when the UI put one there. The
    // clip before it on the same track is found here, by adjacency - the UI
    // only says what it wants, never how to overlap decoders.
    std::optional<TransitionSpec> transition;
    // Video opacity ramp up from the clip's start, in seconds. Set by
    // transition resolution below, never by the UI directly.
    genesis::core::Rational video_fade_in = genesis::core::Rational::ZERO;
    // The source's pixel width, when the UI knows it. What makes an
    // aspect-correct decode possible - absent, the frame is filled edge to
    // edge the way it always was.
    std::optional<std::uint32_t> media_width;
    // The source's pixel height; see `media_width`.
    std::optional<std::uint32_t> media_height;
    // Whether the file carries an audio stream, when the UI knows (the
    // document records it at import). Absent falls back to probing, so an
    // older caller still exports correctly - just slower to start.
    std::optional<bool> has_audio;
    // The background taken away by a mask, as the document holds it.
    // Rendered only with `mask_dir`: a cutout whose masks are nowhere yet
    // leaves the picture whole.
    std::optional<genesis::project::Cutout> cutout;
    // Draw the cutout tinted over the whole picture instead of cutting
    // it: the view while the clip's brushes are in use. Preview only.
    bool highlighted = false;
    // Where this clip's media has its masks - see `concat_vision::mask_dir`
    // - or empty when the flattener had no project folder to name it by.
    std::string mask_dir;
    // The facts the engine needs to find and step this clip's masks: the root
    // they live under, the source they were keyed by, the subject/model, and
    // the decimation stride. Present only when the clip has a cutout and the
    // flattener was handed a mask root; a cutout with no spec renders whole.
    std::optional<genesis::ai::vision::MaskSpec> mask;

    bool operator==(const ExportClip &) const = default;

    // A clip with nothing set but what places it: no file, unity gain,
    // scale and speed, no fades, chains, keys or masks. Every literal in
    // the workspace starts here and sets the few fields it means to, so a
    // field added to this type is added in one place.
    static ExportClip blank(ClipKind kind, genesis::core::Rational start,
                            genesis::core::Rational duration, std::size_t track);
};

// Everything a full export needs: the destination, the output format, and
// the flattened clip list.
struct ExportRequest
{
    // The file to write. Siblings named `.{stem}.concat-*` are used as
    // scratch during the render and removed afterwards.
    std::string output;
    // Output frame width in pixels.
    std::uint32_t width = 0;
    // Output frame height in pixels.
    std::uint32_t height = 0;
    // Frame rate numerator - an exact fraction, so 29.97 stays 30000/1001.
    std::int64_t rate_num = 30;
    // Frame rate denominator; see `rate_num`.
    std::int64_t rate_den = 1;
    // Constant rate factor. Lower is better quality and a bigger file.
    std::uint8_t crf = 0;
    // The x264 speed/size preset name, e.g. "medium".
    std::string preset;
    // Ten bits a channel rather than eight.
    bool ten_bit = false;
    // Target bitrate in kbps, used when `rate_mode` is CBR. Zero means
    // "not set" and the encoder falls back to VBR.
    std::uint32_t bitrate_kbps = 0;
    // The levels the file is written in and tagged with, by name:
    // "limited" (16-235, what every player expects) or "full" (0-255).
    // Limited when a request does not say.
    // https://github.com/jub0t/Concat/issues/103
    genesis::project::ColorRange color_range = genesis::project::ColorRange::Limited;
    // What the timeline is output in. SDR when a request does not say.
    genesis::project::ColorSpace color_space = genesis::project::ColorSpace::Sdr;
    // An HDR timeline written as HDR - HEVC or AV1 in ten bits, BT.2020
    // with its HLG or PQ - rather than tone-mapped to SDR as the monitor
    // shows it. Nothing for an SDR timeline. False when a request does not
    // say.
    bool hdr = false;
    // The flattened clip list to render.
    std::vector<ExportClip> clips;
};

} // namespace genesis::render
