// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-render/src/transitions.rs inline tests,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "render/Transitions.h"

namespace {

int checks = 0;
int failures = 0;

void check(bool condition, const std::string &what)
{
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

namespace render = genesis::render;

// `mask` returns the incoming picture's weight, 0 the outgoing and 1 the
// incoming, sampled at the middle of the frame unless a test says otherwise.
double at(const std::string &name, double progress, double u = 0.5, double v = 0.5)
{
    const std::optional<render::Shape> shape = render::named(name);
    check(shape.has_value(), name + " is a known shape");
    return shape ? render::mask(*shape, progress, u, v) : -1.0;
}

void check_ends(double start, double end, const std::string &what)
{
    check(start < 0.05, what + ": progress 0 is the outgoing picture");
    check(end > 0.95, what + ": progress 1 is the incoming picture");
}

// Every shape runs from one picture to the other at its four corners, which
// is where the edge shapes are at their most extreme.
void test_every_shape_runs_from_one_picture_to_the_other()
{
    const std::vector<std::string> names = {
        "fade",      "fadeblack",  "fadewhite", "radial",    "circleopen", "circleclose",
        "rectcrop",  "wipeleft",   "wiperight", "wipeup",    "wipedown",   "wipetl",
        "diagbr",    "smoothleft", "horzopen",  "horzclose", "vertopen",   "vertclose",
        "slideleft", "slideright", "slideup",   "slidedown", "coverleft",  "revealright",
        "zoomin",    "pixelize",
    };
    const std::vector<std::pair<double, double>> corners = {
        { 0.01, 0.01 }, { 0.5, 0.5 }, { 0.99, 0.99 }, { 0.2, 0.8 }
    };
    for (const std::string &name : names) {
        for (const auto &[u, v] : corners) {
            check_ends(at(name, 0.0, u, v), at(name, 1.0, u, v),
                       name + " at (" + std::to_string(u) + "," + std::to_string(v) + ")");
        }
    }
}

void test_an_unknown_name_is_refused()
{
    check(!render::named("hlslice").has_value(), "hlslice is refused");
    check(!render::named("").has_value(), "the empty name is refused");
    check(!render::named("squeezeh").has_value(), "squeezeh is refused");
    check(!render::named("vdwind").has_value(), "vdwind is refused");
}

void test_a_crossfade_is_the_mean_at_the_middle()
{
    const double mid = at("fade", 0.5);
    check(mid > 0.45 && mid < 0.55, "fade is 50/50 at the middle");
    // Progress past either end is clamped to that end.
    check(at("fade", -3.0) < 0.05, "fade clamps below 0");
    check(at("fade", 7.0) > 0.95, "fade clamps above 1");
}

void test_a_fade_through_black_switches_at_the_middle()
{
    // Through-black is the base-picture selector, a step at the middle; the
    // black modulation itself is not a scalar mask.
    check(at("fadeblack", 0.25) < 0.05, "fadeblack shows A before the middle");
    check(at("fadeblack", 0.75) > 0.95, "fadeblack shows B after the middle");
}

void test_a_clock_wipe_sweeps_clockwise_from_twelve()
{
    const double top_right = at("radial", 0.5, 0.75, 0.25);
    const double bottom_right = at("radial", 0.5, 0.75, 0.75);
    const double bottom_left = at("radial", 0.5, 0.25, 0.75);
    const double top_left = at("radial", 0.5, 0.25, 0.25);
    check(top_right > 0.95, "top right has turned");
    check(bottom_right > 0.95, "bottom right has turned");
    check(bottom_left < 0.05, "bottom left has not");
    check(top_left < 0.05, "top left has not");
}

void test_an_iris_opens_from_the_centre()
{
    check(at("circleopen", 0.5, 0.5, 0.5) > 0.95, "opening iris turned the centre");
    check(at("circleopen", 0.5, 0.01, 0.01) < 0.05, "opening iris left the corner");
    check(at("circleclose", 0.5, 0.5, 0.5) < 0.05, "closing iris kept the centre");
    check(at("circleclose", 0.5, 0.01, 0.01) > 0.95, "closing iris turned the corner");
}

void test_a_wipe_uncovers_from_the_named_side()
{
    check(at("wipeleft", 0.5, 0.99, 0.5) > 0.95, "wipeleft: right edge first");
    check(at("wipeleft", 0.5, 0.01, 0.5) < 0.05, "wipeleft: left edge last");
    check(at("wiperight", 0.5, 0.01, 0.5) > 0.95, "wiperight: left edge first");
    check(at("wiperight", 0.5, 0.99, 0.5) < 0.05, "wiperight: right edge last");
    check(at("wipedown", 0.5, 0.5, 0.01) > 0.95, "wipedown: top first");
    check(at("wipedown", 0.5, 0.5, 0.99) < 0.05, "wipedown: bottom last");
    check(at("wipetl", 0.5, 0.01, 0.01) > 0.95, "wipetl: top-left first");
    check(at("wipetl", 0.5, 0.99, 0.99) < 0.05, "wipetl: bottom-right last");
}

void test_bars_open_from_the_middle_and_close_onto_it()
{
    check(at("horzopen", 0.5, 0.5, 0.5) > 0.95, "horzopen opened the middle");
    check(at("horzopen", 0.5, 0.5, 0.01) < 0.05, "horzopen kept the top");
    check(at("vertclose", 0.5, 0.5, 0.5) < 0.05, "vertclose kept the middle");
    check(at("vertclose", 0.5, 0.01, 0.5) > 0.95, "vertclose turned the side");
}

void test_a_slide_arrives_from_the_far_side()
{
    check(at("slideleft", 0.5, 0.99, 0.5) > 0.95, "slideleft arrived on the right");
    check(at("slideleft", 0.5, 0.01, 0.5) < 0.05, "slideleft still leaving on the left");
    check(at("slideup", 0.5, 0.5, 0.99) > 0.95, "slideup arrived from below");
    check(at("slideup", 0.5, 0.5, 0.01) < 0.05, "slideup left above");
    check(at("coverright", 0.5, 0.01, 0.5) > 0.95, "coverright came in from the left");
    check(at("coverright", 0.5, 0.99, 0.5) < 0.05, "coverright kept the right");
    check(at("revealdown", 0.5, 0.5, 0.01) > 0.95, "revealdown dropped away at the top");
    check(at("revealdown", 0.5, 0.5, 0.99) < 0.05, "revealdown kept the bottom");
}

void test_a_zoom_and_a_mosaic_are_a_crossfade()
{
    // Over flat pictures the spatial shapes move nothing, so their weight is
    // the crossfade's own.
    check(at("zoomin", 0.5) > 0.45 && at("zoomin", 0.5) < 0.55, "zoomin is 50/50 at the middle");
    check(at("pixelize", 0.5) > 0.45 && at("pixelize", 0.5) < 0.55,
          "pixelize is 50/50 at the middle");
}

void test_wipes_are_monotone()
{
    // A hard wipe never goes backwards along the sweep coordinate: as the
    // edge crosses, the weight only ever rises.
    for (const char *name : { "wipeleft", "wiperight", "wipeup", "wipedown", "diagtl", "diagtr" }) {
        double previous = 0.0;
        for (double p = 0.0; p <= 1.0; p += 0.05) {
            const double now = at(name, p);
            check(now >= previous - 1e-9,
                  std::string(name) + " is monotone at " + std::to_string(p));
            previous = now;
        }
    }
}

} // namespace

int main()
{
    test_every_shape_runs_from_one_picture_to_the_other();
    test_an_unknown_name_is_refused();
    test_a_crossfade_is_the_mean_at_the_middle();
    test_a_fade_through_black_switches_at_the_middle();
    test_a_clock_wipe_sweeps_clockwise_from_twelve();
    test_an_iris_opens_from_the_centre();
    test_a_wipe_uncovers_from_the_named_side();
    test_bars_open_from_the_middle_and_close_onto_it();
    test_a_slide_arrives_from_the_far_side();
    test_a_zoom_and_a_mosaic_are_a_crossfade();
    test_wipes_are_monotone();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
