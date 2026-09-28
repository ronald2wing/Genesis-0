// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-render/src/transitions.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include "render/Transitions.h"

#include <algorithm>
#include <cmath>

namespace genesis::render {

namespace {

constexpr double kTau = 6.28318530717958647692528676655900577;
constexpr double kFrac1Sqrt2 = 0.70710678118654752440;
constexpr double kHardWipeSoft = 0.004;
constexpr double kSmoothWipeSoft = 0.15;

// WGSL's smoothstep.
double smoothstep(double edge0, double edge1, double x)
{
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Where a soft edge sits at `progress`: it travels from `-soft` to
// `reach + soft`, so the whole of its blend is off the picture at both ends
// and progress 0 and 1 are the two pictures themselves.
double edge(double progress, double reach, double soft)
{
    return progress * (reach + 2.0 * soft) - soft;
}

// The coordinate behind a wipe's edge, as transitions.rs defines it.
double coord_of(Coord coord, double u, double v)
{
    switch (coord) {
    case Coord::Left:
        return 1.0 - u;
    case Coord::Right:
        return u;
    case Coord::Up:
        return 1.0 - v;
    case Coord::Down:
        return v;
    case Coord::TopLeft:
        return (u + v) / 2.0;
    case Coord::TopRight:
        return (1.0 - u + v) / 2.0;
    case Coord::BottomLeft:
        return (u + 1.0 - v) / 2.0;
    case Coord::BottomRight:
        return (2.0 - u - v) / 2.0;
    }
    return 0.0;
}

bool in_bounds(double x)
{
    return x >= 0.0 && x <= 1.0;
}

Shape wipe(Coord coord, double soft)
{
    Shape shape;
    shape.kind = ShapeKind::Wipe;
    shape.coord = coord;
    shape.soft = soft;
    return shape;
}

Shape iris(bool closing)
{
    Shape shape;
    shape.kind = ShapeKind::Iris;
    shape.closing = closing;
    return shape;
}

Shape bars(Axis axis, bool closing)
{
    Shape shape;
    shape.kind = ShapeKind::Bars;
    shape.axis = axis;
    shape.closing = closing;
    return shape;
}

Shape slide(double dx, double dy, Moving moving)
{
    Shape shape;
    shape.kind = ShapeKind::Slide;
    shape.dx = dx;
    shape.dy = dy;
    shape.moving = moving;
    return shape;
}

Shape single(ShapeKind kind)
{
    Shape shape;
    shape.kind = kind;
    return shape;
}

} // namespace

std::optional<Shape> named(std::string_view xfade)
{
    // The crossfades ride the raw progress rather than an ease, so every
    // dissolve alias collapses onto one shape.
    if (xfade == "fade" || xfade == "dissolve" || xfade == "fadegrays" || xfade == "fadefast"
        || xfade == "fadeslow" || xfade == "distance" || xfade == "hblur") {
        return single(ShapeKind::Cross);
    }
    if (xfade == "fadeblack") {
        return single(ShapeKind::ThroughBlack);
    }
    if (xfade == "fadewhite") {
        return single(ShapeKind::ThroughWhite);
    }
    if (xfade == "radial") {
        return single(ShapeKind::Clock);
    }
    if (xfade == "circleopen") {
        return iris(false);
    }
    if (xfade == "circleclose" || xfade == "circlecrop") {
        return iris(true);
    }
    if (xfade == "rectcrop") {
        Shape shape;
        shape.kind = ShapeKind::Box;
        shape.closing = true;
        return shape;
    }
    if (xfade == "wipeleft")
        return wipe(Coord::Left, kHardWipeSoft);
    if (xfade == "wiperight")
        return wipe(Coord::Right, kHardWipeSoft);
    if (xfade == "wipeup")
        return wipe(Coord::Up, kHardWipeSoft);
    if (xfade == "wipedown")
        return wipe(Coord::Down, kHardWipeSoft);
    if (xfade == "wipetl" || xfade == "diagtl") {
        return wipe(Coord::TopLeft, kHardWipeSoft);
    }
    if (xfade == "wipetr" || xfade == "diagtr") {
        return wipe(Coord::TopRight, kHardWipeSoft);
    }
    if (xfade == "wipebl" || xfade == "diagbl") {
        return wipe(Coord::BottomLeft, kHardWipeSoft);
    }
    if (xfade == "wipebr" || xfade == "diagbr") {
        return wipe(Coord::BottomRight, kHardWipeSoft);
    }
    if (xfade == "smoothleft")
        return wipe(Coord::Left, kSmoothWipeSoft);
    if (xfade == "smoothright")
        return wipe(Coord::Right, kSmoothWipeSoft);
    if (xfade == "smoothup")
        return wipe(Coord::Up, kSmoothWipeSoft);
    if (xfade == "smoothdown")
        return wipe(Coord::Down, kSmoothWipeSoft);
    if (xfade == "horzopen")
        return bars(Axis::Y, false);
    if (xfade == "horzclose")
        return bars(Axis::Y, true);
    if (xfade == "vertopen")
        return bars(Axis::X, false);
    if (xfade == "vertclose")
        return bars(Axis::X, true);
    // A slide's direction is where the pictures go: left means the incoming
    // one enters from the right.
    if (xfade == "slideleft")
        return slide(-1.0, 0.0, Moving::Both);
    if (xfade == "slideright")
        return slide(1.0, 0.0, Moving::Both);
    if (xfade == "slideup")
        return slide(0.0, -1.0, Moving::Both);
    if (xfade == "slidedown")
        return slide(0.0, 1.0, Moving::Both);
    if (xfade == "coverleft")
        return slide(-1.0, 0.0, Moving::Incoming);
    if (xfade == "coverright")
        return slide(1.0, 0.0, Moving::Incoming);
    if (xfade == "coverup")
        return slide(0.0, -1.0, Moving::Incoming);
    if (xfade == "coverdown")
        return slide(0.0, 1.0, Moving::Incoming);
    if (xfade == "revealleft")
        return slide(-1.0, 0.0, Moving::Outgoing);
    if (xfade == "revealright")
        return slide(1.0, 0.0, Moving::Outgoing);
    if (xfade == "revealup")
        return slide(0.0, -1.0, Moving::Outgoing);
    if (xfade == "revealdown")
        return slide(0.0, 1.0, Moving::Outgoing);
    if (xfade == "zoomin") {
        return single(ShapeKind::Zoom);
    }
    if (xfade == "pixelize") {
        return single(ShapeKind::Blocks);
    }
    return std::nullopt;
}

double mask(const Shape &shape, double progress, double u, double v)
{
    const double p = std::clamp(progress, 0.0, 1.0);
    // Most shapes ease the way their shaders do; the crossfades ride the raw
    // progress, which is already the ramp the export planned.
    const double eased = smoothstep(0.0, 1.0, p);

    switch (shape.kind) {
    case ShapeKind::Cross:
        return p;
    case ShapeKind::ThroughBlack:
    case ShapeKind::ThroughWhite:
        return p < 0.5 ? 0.0 : 1.0;
    case ShapeKind::Clock: {
        // Twelve o'clock is straight up; the hand sweeps clockwise.
        double angle = std::atan2(u - 0.5, -(v - 0.5));
        if (angle < 0.0) {
            angle += kTau;
        }
        return angle < eased * kTau ? 1.0 : 0.0;
    }
    case ShapeKind::Iris: {
        const double d = std::sqrt((u - 0.5) * (u - 0.5) + (v - 0.5) * (v - 0.5)) / kFrac1Sqrt2;
        // The edge overshoots its softness at both ends, so progress 0 is
        // exactly the outgoing picture and 1 exactly the incoming.
        const double radius = edge(shape.closing ? 1.0 - eased : eased, 1.15, 0.05);
        const double m = smoothstep(radius - 0.05, radius + 0.05, d);
        return shape.closing ? m : 1.0 - m;
    }
    case ShapeKind::Box: {
        const double d = std::max(std::abs(u - 0.5), std::abs(v - 0.5)) * 2.0;
        const double radius = edge(shape.closing ? 1.0 - eased : eased, 1.05, 0.03);
        const double m = smoothstep(radius - 0.03, radius + 0.03, d);
        return shape.closing ? m : 1.0 - m;
    }
    case ShapeKind::Wipe: {
        const double at = edge(eased, 1.0, shape.soft);
        const double m = smoothstep(at - shape.soft, at + shape.soft, coord_of(shape.coord, u, v));
        return 1.0 - m;
    }
    case ShapeKind::Bars: {
        const double d = shape.axis == Axis::X ? std::abs(u - 0.5) * 2.0 : std::abs(v - 0.5) * 2.0;
        const double at = edge(shape.closing ? 1.0 - eased : eased, 1.0, 0.02);
        const double m = smoothstep(at - 0.02, at + 0.02, d);
        return shape.closing ? m : 1.0 - m;
    }
    case ShapeKind::Slide: {
        // Both pictures travel along (dx, dy): the incoming one is still
        // `1 - eased` short of its place, the outgoing one has gone
        // `eased` past its own. Which picture owns the pixel is which
        // sample point is still on screen.
        const double tu = u + shape.dx * (1.0 - eased);
        const double tv = v + shape.dy * (1.0 - eased);
        const double fu = u - shape.dx * eased;
        const double fv = v - shape.dy * eased;
        switch (shape.moving) {
        case Moving::Both:
        case Moving::Incoming:
            return in_bounds(tu) && in_bounds(tv) ? 1.0 : 0.0;
        case Moving::Outgoing:
            return in_bounds(fu) && in_bounds(fv) ? 0.0 : 1.0;
        }
        return 0.0;
    }
    case ShapeKind::Zoom:
        return eased;
    case ShapeKind::Blocks:
        return p;
    }
    return p;
}

} // namespace genesis::render
