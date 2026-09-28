// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/commands/mod.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Property commands: keys, speed, transform, cutout and patch edits.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "project/model/Cutout.h"
#include "project/model/Keyframe.h"
#include "project/model/Speed.h"

#include "project/command/ClipCommands.h"

namespace genesis::project {

// Gives a clip a speed curve, or takes it away. The source covered is
// held constant, as a speed change does, so the clip's timeline length
// follows the curve's mean; `speed` is kept at that mean.
struct SetClipSpeedCurve
{
    // The clip to retime.
    std::string clip_id;
    // The curve, or None for a constant rate at the current mean.
    std::optional<std::vector<SpeedPoint>> curve;

    bool operator==(const SetClipSpeedCurve &) const = default;
};

// Puts a key on one property at one point of a clip, replacing
// whichever key on that property was already within a hair of it.
//
// The value is the property's own - the number the inspector shows -
// not the relative factor the engine plays; see `model::ClipKey`.
struct SetClipKey
{
    // The clip.
    std::string clip_id;
    // Which property is being keyed.
    KeyProperty property;
    // Where in the clip, `0..=1`.
    double at;
    // The value there.
    double value;
    // How the key is approached from the one before it.
    KeyEase ease;

    bool operator==(const SetClipKey &) const = default;
};

// Takes the key at `at` off one property, if there is one there. The
// other half of the diamond: a filled diamond clicked is this.
struct ClearClipKey
{
    // The clip.
    std::string clip_id;
    // Which property.
    KeyProperty property;
    // Where in the clip the key to remove sits, `0..=1`.
    double at;

    bool operator==(const ClearClipKey &) const = default;
};

// Takes every key off one property, returning it to its constant.
struct ClearClipKeys
{
    // The clip.
    std::string clip_id;
    // Which property.
    KeyProperty property;

    bool operator==(const ClearClipKeys &) const = default;
};

// Puts a key on one parameter of one link of a clip's picture chain,
// replacing whichever key on it was already within a hair of `at`.
// The value is the parameter's own; the package's range is the
// caller's to keep, the model not knowing it.
struct SetEffectKey
{
    // The clip.
    std::string clip_id;
    // Which link, as an index into `video_effects`. Out of range is a
    // no-op.
    std::size_t entry;
    // The parameter's manifest key.
    std::string key;
    // Where in the clip, `0..=1`.
    double at;
    // The value there.
    double value;
    // How the key is approached from the one before it.
    KeyEase ease;

    bool operator==(const SetEffectKey &) const = default;
};

// Takes the key at `at` off one parameter of one link, if there is one.
struct ClearEffectKey
{
    // The clip.
    std::string clip_id;
    // Which link, as an index into `video_effects`.
    std::size_t entry;
    // The parameter's manifest key.
    std::string key;
    // Where in the clip the key to remove sits, `0..=1`.
    double at;

    bool operator==(const ClearEffectKey &) const = default;
};

// Takes every key off one parameter of one link, returning it to the
// value it holds.
struct ClearEffectKeys
{
    // The clip.
    std::string clip_id;
    // Which link, as an index into `video_effects`.
    std::size_t entry;
    // The parameter's manifest key.
    std::string key;

    bool operator==(const ClearEffectKeys &) const = default;
};

// Sets or clears a picture's cutout: the mask that takes its
// background away. Tidied on the way in; see Cutout::tidy.
struct SetClipCutout
{
    // The clip. An unknown id is a no-op.
    std::string clip_id;
    // The cutout, or None to take it off.
    std::optional<Cutout> cutout;

    bool operator==(const SetClipCutout &) const = default;
};

// Paints one stroke onto a clip's cutout. A clip with no cutout gets a
// custom one; an automatic cutout becomes custom, since a stroke is a
// correction to it. A stroke with no points is a no-op.
struct AddCutoutStroke
{
    // The clip. An unknown id is a no-op.
    std::string clip_id;
    // The stroke, in source fractions.
    Stroke stroke;

    bool operator==(const AddCutoutStroke &) const = default;
};

// Applies a ClipPatch: only the fields present change, with the
// clamps documented on the patch. An unknown clip is a no-op.
struct UpdateClip
{
    // The clip to patch.
    std::string clip_id;
    // Which properties change, and to what.
    ClipPatch patch;

    bool operator==(const UpdateClip &) const = default;
};

// Changes playback rate while holding the amount of source covered
// constant - the clip's timeline duration is what stretches, which is
// what makes this a speed change rather than a trim.
struct SetClipSpeed
{
    // The clip to retime. An unknown id is a no-op.
    std::string clip_id;
    // The new rate, clamped into 0.0625..=16 (the engine's range).
    double speed;

    bool operator==(const SetClipSpeed &) const = default;
};

// Turns a clip's playback backwards: true shows the covered source span in
// reverse. A playback direction, not a transform - the clip keeps its
// length, in-point and looks, and the app generates a reversed copy of the
// source span when it builds the timeline.
struct SetClipReverse
{
    // The clip to reverse. An unknown id is a no-op.
    std::string clip_id;
    // Whether the clip plays backwards.
    bool reverse;

    bool operator==(const SetClipReverse &) const = default;
};

// Moves a clip's sound across the stereo field: -1 full left, +1 full
// right, 0 centred. A constant, not a keyable ride - the pan panel is a
// single knob for the whole clip, not a curve.
struct SetClipPan
{
    // The clip to pan. An unknown id is a no-op.
    std::string clip_id;
    // The position, clamped into -1..=1.
    double pan;

    bool operator==(const SetClipPan &) const = default;
};

// Adjusts the picture's placement. Each field is optional so a drag
// can send just the axis it moved; absent fields stay put.
struct SetClipTransform
{
    // The clip to place. An unknown id is a no-op.
    std::string clip_id;
    // New scale, clamped into 0.05..=8.
    std::optional<double> scale;
    // New horizontal offset as a frame-width fraction, clamped to ±3.
    std::optional<double> offset_x;
    // New vertical offset as a frame-height fraction, clamped to ±3.
    std::optional<double> offset_y;
    // New rotation in degrees, wrapped into (-180, 180] so a full drag
    // never accumulates turns.
    std::optional<double> rotation;
    // New width multiplier beyond the scale, clamped into 0.1..=10.
    std::optional<double> stretch_x;
    // New height multiplier, on the same terms.
    std::optional<double> stretch_y;

    bool operator==(const SetClipTransform &) const = default;
};

} // namespace genesis::project
