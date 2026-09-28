// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstddef>
#include <vector>

#include "core/ClipTimeline.h"

namespace genesis::core {

// How a key is approached from the one before it: a CSS timing function, as
// its two control points.
//
// Four numbers rather than a handful of named shapes, because the named shapes
// are four numbers each and a curve editor is not - the panel hands people a
// bezier to drag, and the presets below are what its chips write. The endpoints
// are pinned at (0,0) and (1,1), so only the middle two points are stored, and
// x is clamped into 0..=1 where a legal timing function keeps it.
struct Ease
{
    // First control point's x, 0..=1.
    double x1 = 0.0;
    // First control point's y; may overshoot.
    double y1 = 0.0;
    // Second control point's x, 0..=1.
    double x2 = 1.0;
    // Second control point's y; may overshoot.
    double y2 = 1.0;

    // A straight line.
    static constexpr Ease linear() { return Ease{ 0.0, 0.0, 1.0, 1.0 }; }
    // Starts slow, arrives fast. CSS ease-in.
    static constexpr Ease in() { return Ease{ 0.42, 0.0, 1.0, 1.0 }; }
    // Starts fast, arrives slow. CSS ease-out.
    static constexpr Ease out() { return Ease{ 0.0, 0.0, 0.58, 1.0 }; }
    // Slow at both ends. CSS ease-in-out.
    static constexpr Ease in_out() { return Ease{ 0.42, 0.0, 0.58, 1.0 }; }

    // Whether this is the straight line, to within a hair. What lets the mixer
    // skip resampling a ride that has nothing to resample.
    bool is_linear() const;

    // The eased fraction for a linear one.
    double apply(double t) const;

    bool operator==(const Ease &other) const
    {
        return x1 == other.x1 && y1 == other.y1 && x2 == other.x2 && y2 == other.y2;
    }
};

// Solve a CSS cubic-bezier for y at a given x: Newton first, bisection as the
// fallback where the curve is flat enough that Newton stalls.
//
// The one solver. The window's Curves.ease global reaches it through
// format::bezier_y_at_x, because Slint's expression language has no loops and
// so cannot do this itself; the engine reaches it through Ease::apply on every
// frame of every keyed property. Two callers, one definition - which matters
// here more than most, because a preview whose easing disagreed with the
// export's would disagree invisibly.
double bezier_y_at_x(double x1, double y1, double x2, double y2, double x);

// One key of one property.
struct Key
{
    // Where in the clip, 0..=1.
    double at = 0.0;
    // The value there, in the property's relative terms.
    double value = 0.0;
    // How this key is approached from the previous one.
    Ease ease = Ease::linear();

    bool operator==(const Key &other) const
    {
        return at == other.at && value == other.value && ease == other.ease;
    }
};

// The keys of one property, sorted by at.
class KeyTrack
{
public:
    KeyTrack() = default;
    // A track through these keys, sorted.
    explicit KeyTrack(std::vector<Key> keys);

    // The keys, sorted.
    const std::vector<Key> &keys() const { return keys_; }

    // True with no keys: the property is the clip's own.
    bool is_empty() const { return keys_.empty(); }

    // The value at x: rest with no keys, the first key's before it, the last
    // key's after it, and eased between neighbours.
    double value_at(double x, double rest) const;

    // The same ride, with every eased segment replaced by steps straight ones
    // through the same points.
    //
    // For callers that can interpolate but cannot ease. The mixer is the one
    // that needs it: a clip's gain ride becomes an FFmpeg volume expression,
    // and an expression can lerp but cannot run the Newton solve a bezier
    // wants. Sampling here rather than there keeps the easing in the one place
    // that understands it, and leaves the expression generator with nothing
    // but straight lines to write.
    //
    // Linear segments are passed through untouched, so a ride nobody has eased
    // costs nothing and reads identically on both sides.
    KeyTrack resample(std::size_t steps) const;

    bool operator==(const KeyTrack &other) const { return keys_ == other.keys_; }

private:
    std::vector<Key> keys_;
};

// Every animatable property's keys.
struct Animation
{
    // A factor on the clip's scale; 1 is as placed.
    KeyTrack scale;
    // Added to the clip's horizontal offset, in frame widths.
    KeyTrack offset_x;
    // Added to the clip's vertical offset, in frame heights.
    KeyTrack offset_y;
    // Added to the clip's rotation, in degrees.
    KeyTrack rotation;
    // A factor on the clip's opacity; 1 is as set.
    KeyTrack opacity;
    // A factor on the clip's gain; 1 is as mixed. Sound rather than picture,
    // but the same shape of fact - a value that changes over the clip - so it
    // is a track here and not a second mechanism somewhere else. Nothing in
    // the compositor reads it; the mixer does.
    KeyTrack volume;

    // True when no property has a key.
    bool is_empty() const;

    // The clip's transform at x, a fraction of its length.
    Transform transform_at(Transform base, double x) const;

    // The clip's opacity at x.
    float opacity_at(float base, double x) const;

    bool operator==(const Animation &other) const
    {
        return scale == other.scale && offset_x == other.offset_x && offset_y == other.offset_y
                && rotation == other.rotation && opacity == other.opacity && volume == other.volume;
    }
};

} // namespace genesis::core
