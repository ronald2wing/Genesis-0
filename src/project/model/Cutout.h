// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/model.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Cutout model: cutout modes, subjects, brush strokes, feathering and cropping.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "core/ClipTimeline.h"

namespace genesis::project {

using genesis::core::Rational;

// The two ways a cutout decides what stays.
// On disk: lowercase.
enum class CutoutMode {
    // The model's mask as it is.
    Auto,
    // The model's mask, then the strokes over it.
    Custom,
};

// What a cutout keeps: the person the matting model finds, the one
// thing the picture is of whatever it is, or whichever of those the
// analysis decides fits the footage.
// On disk: lowercase.
enum class Subject {
    // A person when the person model finds one, the object otherwise.
    Auto,
    // The person model, always.
    Person,
    // The object model, always.
    Object,
};

// The default, left out of the document.
constexpr bool is_auto(Subject subject)
{
    return subject == Subject::Auto;
}

// The name the mask cache is keyed by.
constexpr std::string_view key(Subject subject)
{
    switch (subject) {
    case Subject::Auto:
        return "auto";
    case Subject::Person:
        return "person";
    case Subject::Object:
        return "object";
    }
    return "auto";
}

// The four brushes of a custom cutout.
// On disk: camelCase.
enum class BrushTool {
    // Keeps what the model thought might be the subject under the stroke.
    SmartBrush,
    // Keeps everything under the stroke.
    Brush,
    // Removes what the model was unsure of under the stroke.
    SmartEraser,
    // Removes everything under the stroke.
    Eraser,
};

// The edge softness a cutout starts with.
inline constexpr double DEFAULT_FEATHER = 0.01;
// The widest an edge may be softened.
inline constexpr double MAX_FEATHER = 0.1;
// The brush's narrowest and widest, as fractions of the picture's width.
inline constexpr double MIN_BRUSH = 0.005;
// See `MIN_BRUSH`.
inline constexpr double MAX_BRUSH = 0.5;

// One brush stroke over a cutout: which tool, how wide, and where it went.
// Points are fractions of the source picture, `(0, 0)` its top-left, so a
// stroke survives a change of output size, a crop or a flip.
// On disk: camelCase.
struct Stroke
{
    // What the stroke does to the mask beneath it.
    BrushTool tool;
    // The brush's diameter as a fraction of the picture's width.
    double size;
    // The path, as `[x, y]` fractions.
    std::vector<std::array<double, 2>> points;
    // The source instant, in seconds, the stroke was painted at: the
    // frame a smart brush reads the thing under it from. Absent on
    // strokes from before it was recorded, which paint as plain discs.
    // Left out of the document when absent.
    std::optional<Rational> at;

    bool operator==(const Stroke &) const = default;

    // Whether the stroke's tool reads the picture rather than painting
    // a disc: the smart brush and the smart eraser.
    bool is_smart() const
    {
        return tool == BrushTool::SmartBrush || tool == BrushTool::SmartEraser;
    }

    // The size held to its range and the points to the picture; `None`
    // for a stroke with no points left.
    std::optional<Stroke> tidy() const
    {
        Stroke out = *this;
        out.size = std::isfinite(out.size) ? std::clamp(out.size, MIN_BRUSH, MAX_BRUSH) : MIN_BRUSH;
        if (out.at) {
            *out.at = std::max(*out.at, Rational::ZERO);
        }
        std::erase_if(out.points, [](const std::array<double, 2> &point) {
            return !std::isfinite(point[0]) || !std::isfinite(point[1]);
        });
        for (std::array<double, 2> &point : out.points) {
            point[0] = std::clamp(point[0], -0.5, 1.5);
            point[1] = std::clamp(point[1], -0.5, 1.5);
        }
        if (out.points.empty()) {
            return std::nullopt;
        }
        return out;
    }
};

// How a picture's background is taken away when there is no key colour
// to remove: a person mask found by the cutout model, alone or corrected
// by hand.
// On disk: camelCase.
struct Cutout
{
    // Automatic keeps what the model finds; custom adds the strokes.
    CutoutMode mode;
    // What the model looks for. Left out of the document when auto.
    Subject subject = Subject::Auto;
    // How far the edge is softened, as a fraction of the picture's width.
    double feather = DEFAULT_FEATHER;
    // Corrections painted on the monitor, in the order they were made.
    // Left out of the document when empty.
    std::vector<Stroke> strokes;

    bool operator==(const Cutout &) const = default;

    // An automatic cutout with the default edge.
    static Cutout automatic()
    {
        return Cutout{ CutoutMode::Auto, Subject::Auto, DEFAULT_FEATHER, { } };
    }

    // The feather held to its range, every stroke to its own, and strokes
    // with nowhere to go dropped.
    Cutout tidy() const
    {
        Cutout out = *this;
        out.feather = std::isfinite(out.feather) ? std::clamp(out.feather, 0.0, MAX_FEATHER)
                                                 : DEFAULT_FEATHER;
        std::vector<Stroke> kept;
        kept.reserve(out.strokes.size());
        for (const Stroke &stroke : out.strokes) {
            std::optional<Stroke> tidied = stroke.tidy();
            if (tidied) {
                kept.push_back(std::move(*tidied));
            }
        }
        out.strokes = std::move(kept);
        return out;
    }
};

// A crop: what is taken off each edge, as fractions of the source.
// On disk: camelCase.
struct Crop
{
    // Off the left, `0..1`.
    double left = 0.0;
    // Off the top.
    double top = 0.0;
    // Off the right.
    double right = 0.0;
    // Off the bottom.
    double bottom = 0.0;

    bool operator==(const Crop &) const = default;

    // True when nothing is cut.
    bool is_none() const { return left <= 0.0 && top <= 0.0 && right <= 0.0 && bottom <= 0.0; }

    // Each edge held to `0..=0.9`, and a pair that would meet pulled back
    // so at least a tenth of the picture is left.
    Crop tidy() const
    {
        Crop out{
            std::clamp(left, 0.0, 0.9),
            std::clamp(top, 0.0, 0.9),
            std::clamp(right, 0.0, 0.9),
            std::clamp(bottom, 0.0, 0.9),
        };
        if (out.left + out.right > 0.9) {
            out.right = std::max(0.9 - out.left, 0.0);
        }
        if (out.top + out.bottom > 0.9) {
            out.bottom = std::max(0.9 - out.top, 0.0);
        }
        return out;
    }
};

} // namespace genesis::project
