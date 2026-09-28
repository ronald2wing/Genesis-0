// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Keyframe model: keyable properties, easing presets and one user-set key.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>

#include "core/Animation.h"

namespace genesis::project {

// Which property a user-set key belongs to.
//
// Five of the six are the picture's; the sixth is the mix's. They are one
// enum because a key is a key - the panel that sets them, the commands that
// carry them and the document that stores them do not care which side of
// the clip a value ends up on, and only the thing that finally reads them
// does.
// On disk: camelCase.
enum class KeyProperty {
    // Clip::scale.
    Scale,
    // Clip::offset_x.
    OffsetX,
    // Clip::offset_y.
    OffsetY,
    // Clip::rotation.
    Rotation,
    // Clip::opacity.
    Opacity,
    // Clip::volume.
    Volume,
};

// The name this property carries in a document and across the export
// boundary. Matches the ExportKey::property vocabulary.
constexpr std::string_view name(KeyProperty property)
{
    switch (property) {
    case KeyProperty::Scale:
        return "scale";
    case KeyProperty::OffsetX:
        return "offsetX";
    case KeyProperty::OffsetY:
        return "offsetY";
    case KeyProperty::Rotation:
        return "rotation";
    case KeyProperty::Opacity:
        return "opacity";
    case KeyProperty::Volume:
        return "volume";
    }
    return "scale";
}

// The property of that name, or nullopt.
constexpr std::optional<KeyProperty> from_name(std::string_view name)
{
    if (name == "scale") {
        return KeyProperty::Scale;
    }
    if (name == "offsetX") {
        return KeyProperty::OffsetX;
    }
    if (name == "offsetY") {
        return KeyProperty::OffsetY;
    }
    if (name == "rotation") {
        return KeyProperty::Rotation;
    }
    if (name == "opacity") {
        return KeyProperty::Opacity;
    }
    if (name == "volume") {
        return KeyProperty::Volume;
    }
    return std::nullopt;
}

// Every property that can be keyed, in the order a panel lists them.
inline constexpr std::array<KeyProperty, 6> key_property_all = {
    KeyProperty::Scale,    KeyProperty::OffsetX, KeyProperty::OffsetY,
    KeyProperty::Rotation, KeyProperty::Opacity, KeyProperty::Volume,
};

// How a key is approached from the one before it: a CSS timing function's
// two control points, [x1, y1, x2, y2].
//
// The document's spelling of genesis::core::Ease, which is what it becomes.
// Four numbers rather than a named shape because the curve editor hands
// people a bezier to drag and the four named shapes are only its preset
// chips - see linear() and friends.
//
// On disk: either a preset name ("in"/"out"/"inOut") or an array of four
// numbers; anything else reads as LINEAR.
struct KeyEase
{
    // A straight line when unset.
    std::array<double, 4> values{ 0.0, 0.0, 1.0, 1.0 };

    bool operator==(const KeyEase &) const = default;

    // A straight line.
    static constexpr KeyEase linear()
    {
        return KeyEase{ std::array<double, 4>{ 0.0, 0.0, 1.0, 1.0 } };
    }
    // Starts slow, arrives fast.
    static constexpr KeyEase in()
    {
        return KeyEase{ std::array<double, 4>{ 0.42, 0.0, 1.0, 1.0 } };
    }
    // Starts fast, arrives slow.
    static constexpr KeyEase out()
    {
        return KeyEase{ std::array<double, 4>{ 0.0, 0.0, 0.58, 1.0 } };
    }
    // Slow at both ends.
    static constexpr KeyEase in_out()
    {
        return KeyEase{ std::array<double, 4>{ 0.42, 0.0, 0.58, 1.0 } };
    }

    // The four numbers, with the two x values clamped where a legal timing
    // function keeps them and anything unreadable falling back to linear.
    constexpr KeyEase sane() const
    {
        for (double value : values) {
            if (!std::isfinite(value)) {
                return linear();
            }
        }
        return KeyEase{ std::array<double, 4>{
                std::clamp(values[0], 0.0, 1.0),
                values[1],
                std::clamp(values[2], 0.0, 1.0),
                values[3],
        } };
    }

    // Whether this is one of the presets, to within a hair. What lights a
    // chip in the panel.
    constexpr bool is(const KeyEase &other) const
    {
        return std::abs(values[0] - other.values[0]) < 0.005
                && std::abs(values[1] - other.values[1]) < 0.005
                && std::abs(values[2] - other.values[2]) < 0.005
                && std::abs(values[3] - other.values[3]) < 0.005;
    }

    // The one conversion into what the engine plays. Sanitised on the way,
    // so a hand-edited document cannot hand the solver an x outside 0..=1,
    // where a cubic bezier stops being a function of x and the Newton
    // iteration stops converging.
    explicit operator genesis::core::Ease() const
    {
        const KeyEase ease = sane();
        return genesis::core::Ease{ ease.values[0], ease.values[1], ease.values[2],
                                    ease.values[3] };
    }
};

// The four presets and what to call them, in the order the panel's chips sit
// in. A free variable rather than a static member: an in-class array cannot
// call the class's own static functions before the type is complete.
inline constexpr std::array<std::pair<std::string_view, KeyEase>, 4> KEY_EASE_PRESETS = { {
        { "Linear", KeyEase::linear() },
        { "In", KeyEase::in() },
        { "Out", KeyEase::out() },
        { "In · Out", KeyEase::in_out() },
} };

// How near two keys on one property have to be to count as the same key.
//
// A fraction and not a duration, because that is what `at` is. Two
// thousandths of a clip is under a frame for anything up to about twenty
// seconds, which is the length where "the playhead is on that key" stops
// being a question a user can answer by looking.
inline constexpr double KEY_EPSILON = 0.002;

// One user-set key: a property, a point in the clip, and the value there.
//
// The value is *absolute* - the number the inspector shows, in the
// property's own units - and not the relative factor the engine's
// `animate::Key` carries. Storing what the user typed is what keeps a key
// meaning the same thing after the clip's own scale or gain is changed
// underneath it; the conversion to relative happens on the way out, when the
// keys are flattened for export.
// On disk: camelCase.
struct ClipKey
{
    // Which property.
    KeyProperty property;
    // Where in the clip, as a fraction of its timeline length, `0..=1`.
    double at;
    // The value there, in the property's own units.
    double value;
    // How this key is approached from the previous one. Absent means linear.
    KeyEase ease;

    bool operator==(const ClipKey &) const = default;
};

} // namespace genesis::project
