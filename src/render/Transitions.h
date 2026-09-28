// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-render/src/transitions.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <optional>
#include <string_view>

namespace genesis::render {

// The transition shapes a compositor without shaders can still draw: the
// FFmpeg `xfade` name a transition package declares as its degrade-to, drawn
// in plain arithmetic over the two finished pictures. `named` turns an xfade
// name into a shape; a name this file does not know is nullopt, and the
// compositor keeps the dissolve the incoming clip already carries - the same
// answer as before there were shapes at all.

// Which way a wipe's edge travels, as the coordinate that is behind it.
enum class Coord {
    Left,
    Right,
    Up,
    Down,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

// The axis a bars wipe opens or closes along.
enum class Axis { X, Y };

// Which pictures a slide moves.
enum class Moving {
    // A push: both pictures, in step.
    Both,
    // A cover: the incoming picture over a still outgoing one.
    Incoming,
    // A reveal: the outgoing picture off a still incoming one.
    Outgoing,
};

// The shape a name means. The payload fields are only meaningful for the
// kinds that name them: `closing` for Iris/Box/Bars, `coord` and `soft` for
// Wipe, `axis` for Bars, and `dx`/`dy`/`moving` for Slide.
enum class ShapeKind {
    Cross,
    ThroughBlack,
    ThroughWhite,
    Clock,
    Iris,
    Box,
    Wipe,
    Bars,
    Slide,
    Zoom,
    Blocks,
};

// A shape value, built by `named`.
struct Shape
{
    ShapeKind kind = ShapeKind::Cross;
    bool closing = false;
    Coord coord = Coord::Left;
    double soft = 0.0;
    Axis axis = Axis::X;
    double dx = 0.0;
    double dy = 0.0;
    Moving moving = Moving::Both;

    bool operator==(const Shape &) const = default;
};

// The shape an FFmpeg `xfade` name means here, or nullopt for a name this
// file cannot draw.
std::optional<Shape> named(std::string_view xfade);

// The per-pixel weight of the incoming picture at `progress` through the
// shape, `0..=1`: 0 is all the outgoing picture, 1 all the incoming one, and
// the compositor draws `mix(outgoing, incoming, mask)`. `progress` is clamped
// to 0..=1; `(u, v)` is the pixel's normalised position, each 0..=1, sampled
// the way the shaders' samplers clamp at the edges.
//
// The shapes that are pure masks (Cross, Clock, Iris, Box, Wipe, Bars, Zoom,
// Blocks) return their blend factor exactly; a Slide returns the binary
// 0/1 of which picture owns the pixel. ThroughBlack and ThroughWhite are not
// mixes of the two pictures - they modulate one base picture toward black or
// white - so their weight is the base-picture selector, a step at the middle
// (0 while the outgoing picture shows, 1 once the incoming one does); the
// black/white modulation itself is the compositor's separate concern, and is
// not carried by this scalar.
double mask(const Shape &shape, double progress, double u, double v);

} // namespace genesis::render
