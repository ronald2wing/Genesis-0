// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-core inline tests (retime.rs, frame.rs,
// animate.rs, shader.rs),
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "core/Animation.h"
#include "core/ClipTimeline.h"
#include "core/Frame.h"
#include "core/ShaderPass.h"
#include "core/SpeedCurve.h"

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

void check_near(double got, double want, double tolerance, const std::string &what)
{
    check(std::abs(got - want) <= tolerance,
          what + " (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

using namespace genesis::core;

// --- retime.rs -------------------------------------------------------------

void test_flat_curve_is_constant_speed()
{
    const auto curve = SpeedCurve::create({ { 0.0, 2.0 }, { 1.0, 2.0 } });
    check(curve.has_value(), "flat curve is accepted");
    check_near(curve->speed_at(0.3), 2.0, 1e-12, "flat speed");
    check_near(curve->consumed(0.5), 1.0, 1e-12, "flat consumed");
    check_near(curve->mean(), 2.0, 1e-12, "flat mean");
}

void test_ends_anchored_and_area_exact()
{
    const auto curve = SpeedCurve::create({ { 0.25, 1.0 }, { 0.75, 3.0 } });
    check(curve.has_value(), "ramp curve is accepted");
    const auto &points = curve->points();
    check(points.front().first == 0.0 && points.front().second == 1.0, "first point anchored");
    check(points.back().first == 1.0 && points.back().second == 3.0, "last point anchored");
    check_near(curve->speed_at(0.5), 2.0, 1e-12, "midpoint speed");
    check_near(curve->mean(), 0.25 + 1.0 + 0.75, 1e-12, "trapezoid area");
    check_near(curve->consumed(0.5), 0.25 + 0.25 * 1.5, 1e-12, "consumed at midpoint");
}

void test_pieces_add_up_to_whole()
{
    const auto curve = SpeedCurve::create({ { 0.0, 0.5 }, { 0.5, 4.0 }, { 1.0, 0.5 } });
    check(curve.has_value(), "three-point curve is accepted");
    const auto pieces = curve->pieces(8);
    check(pieces.size() == 8, "eight pieces");
    double total = 0.0;
    for (const auto &[x0, x1, consumed, mean] : pieces) {
        (void)consumed;
        total += (x1 - x0) * mean;
    }
    check_near(total, curve->mean(), 1e-9, "pieces sum to mean");
    check_near(std::get<2>(pieces[3]), curve->consumed(3.0 / 8.0), 1e-12, "piece consumed matches");
}

void test_nonsense_is_tidied_or_refused()
{
    check(!SpeedCurve::create({ }).has_value(), "empty curve refused");
    const auto curve = SpeedCurve::create({ { 2.0, 0.0 }, { -1.0, 100.0 } });
    check(curve.has_value(), "out-of-range curve tidied");
    check(curve->points().front().second == SpeedCurve::MAX_SPEED, "first speed clamped to max");
    check(curve->points().back().second == SpeedCurve::MIN_SPEED, "last speed clamped to min");
}

// --- frame.rs --------------------------------------------------------------

void test_black_is_opaque()
{
    const Frame frame = Frame::black(2, 2);
    const auto pixel = frame.pixel(0, 0);
    check(pixel.has_value() && (*pixel)[0] == 0 && (*pixel)[1] == 0 && (*pixel)[2] == 0
                  && (*pixel)[3] == 255,
          "black pixel is opaque black");
    check(frame.pixels().size() == 16, "2x2 frame is 16 bytes");
}

void test_transparent_is_clear()
{
    const auto pixel = Frame::transparent(1, 1).pixel(0, 0);
    check(pixel.has_value() && (*pixel)[3] == 0, "transparent alpha is zero");
}

void test_rejects_wrong_buffer_size()
{
    check(!Frame::from_rgba(2, 2, std::vector<std::uint8_t>(15)).has_value(),
          "short buffer refused");
    check(Frame::from_rgba(2, 2, std::vector<std::uint8_t>(16)).has_value(),
          "exact buffer accepted");
}

void test_out_of_bounds_is_none()
{
    Frame frame = Frame::black(2, 2);
    check(!frame.pixel(2, 0).has_value(), "x out of bounds is none");
    check(!frame.pixel(0, 2).has_value(), "y out of bounds is none");
    frame.set_pixel(99, 99, { 255, 0, 0, 255 }); // ignored, must not crash
    check(true, "out-of-bounds write ignored");
}

void test_set_and_read_pixel()
{
    Frame frame = Frame::black(4, 3);
    frame.set_pixel(3, 2, { 1, 2, 3, 4 });
    const auto pixel = frame.pixel(3, 2);
    check(pixel.has_value() && (*pixel)[0] == 1 && (*pixel)[1] == 2 && (*pixel)[2] == 3
                  && (*pixel)[3] == 4,
          "pixel round-trips");
    check(!frame.pixel(2, 3).has_value(), "transposed access is none");
}

void test_rows_are_tightly_packed()
{
    const Frame frame = Frame::black(5, 4);
    const auto pixels = frame.pixels();
    check(pixels.size() == 4 * 20, "four rows of 20 bytes");
    bool all_twenty = true;
    for (std::size_t row = 0; row < 4; ++row) {
        if (pixels.subspan(row * 20, 20).size() != 20)
            all_twenty = false;
    }
    check(all_twenty, "each row is 20 bytes");
}

// --- time.rs (Rational) ----------------------------------------------------

void test_rational_survives_large_cross_products()
{
    // 4'300'000'000 > 2^32, so its square exceeds 2^64. The old hand-rolled
    // wide multiply aborted on a product this large; the fixed-width
    // multiprecision backend computes it exactly instead.
    const Rational tiny(1, 4'300'000'000LL);
    check(tiny + tiny == Rational(1, 2'150'000'000LL),
          "1/4.3e9 + 1/4.3e9 exact across a >2^64 denominator product");
    check(Rational(4'300'000'000LL, 1) > Rational(1, 4'300'000'000LL),
          "4.3e9 > 1/4.3e9 via a >2^64 cross product");
    check(Rational(1, 4'000'000'000LL) > Rational(1, 4'000'000'001LL), "1/4e9 > 1/(4e9+1)");
}

// --- animate.rs ------------------------------------------------------------

Key key(double at, double value, Ease ease)
{
    return Key{ at, value, ease };
}

void test_track_holds_ends_and_eases_between()
{
    const KeyTrack track({ key(0.5, 1.0, Ease::linear()), key(0.0, 0.0, Ease::linear()) });
    check_near(track.value_at(-1.0, 7.0), 0.0, 1e-12, "before first key");
    check_near(track.value_at(0.25, 7.0), 0.5, 1e-12, "between keys");
    check_near(track.value_at(0.9, 7.0), 1.0, 1e-12, "after last key");
    check_near(KeyTrack().value_at(0.5, 7.0), 7.0, 1e-12, "empty track rests");

    const KeyTrack eased_out({ key(0.0, 0.0, Ease::linear()), key(1.0, 1.0, Ease::out()) });
    check(eased_out.value_at(0.5, 0.0) > 0.5, "ease-out leads");
    const KeyTrack eased_in({ key(0.0, 0.0, Ease::linear()), key(1.0, 1.0, Ease::in()) });
    check(eased_in.value_at(0.5, 0.0) < 0.5, "ease-in lags");
}

void test_animation_is_relative_to_clip()
{
    Animation animation;
    animation.scale = KeyTrack({ key(0.0, 0.5, Ease::linear()), key(1.0, 1.0, Ease::linear()) });
    animation.offset_x = KeyTrack({ key(0.0, 0.5, Ease::linear()), key(1.0, 0.0, Ease::linear()) });
    animation.opacity = KeyTrack({ key(0.0, 0.0, Ease::linear()), key(0.5, 1.0, Ease::linear()) });

    Transform base;
    base.scale = 2.0;
    base.offset_x = 0.1;
    base.offset_y = -0.2;
    base.rotation = 10.0;

    const Transform mid = animation.transform_at(base, 0.5);
    check_near(mid.scale, 1.5, 1e-12, "scale is relative");
    check_near(mid.offset_x, 0.35, 1e-12, "offset is additive");
    check_near(mid.offset_y, -0.2, 1e-12, "untracked offset passes through");
    check_near(mid.rotation, 10.0, 1e-12, "untracked rotation passes through");
    check_near(animation.opacity_at(0.8f, 0.25), 0.4, 1e-6, "opacity mid");
    check_near(animation.opacity_at(0.8f, 0.9), 0.8, 1e-6, "opacity held");
    check(!animation.is_empty(), "keyed animation is not empty");
    check(Animation().is_empty(), "default animation is empty");
}

void test_eases_bend_the_way_their_names_say()
{
    const Ease eases[] = { Ease::linear(), Ease::in(), Ease::out(), Ease::in_out() };
    for (const Ease &ease : eases) {
        check_near(ease.apply(0.0), 0.0, 1e-6, "ease pinned at 0");
        check_near(ease.apply(1.0), 1.0, 1e-6, "ease pinned at 1");
    }
    check(Ease::linear().is_linear(), "linear is linear");
    check_near(Ease::linear().apply(0.37), 0.37, 1e-12, "linear is identity");
    check(!Ease::in().is_linear(), "ease-in is not linear");

    check(Ease::in().apply(0.5) < 0.5, "ease-in starts slow");
    check(Ease::out().apply(0.5) > 0.5, "ease-out starts fast");
    check_near(Ease::in_out().apply(0.5), 0.5, 1e-6, "in-out is symmetric");
    check(Ease::in_out().apply(0.25) < 0.25, "in-out lags early");
    check(Ease::in_out().apply(0.75) > 0.75, "in-out leads late");

    check_near(Ease::in().apply(-1.0), 0.0, 1e-12, "below range held");
    check_near(Ease::in().apply(2.0), 1.0, 1e-12, "above range held");
}

void test_resampling_follows_the_curve_it_flattens()
{
    const KeyTrack track({ key(0.0, 0.0, Ease::linear()), key(1.0, 4.0, Ease::in_out()) });
    const KeyTrack flat = track.resample(12);

    bool all_linear = true;
    for (const Key &k : flat.keys()) {
        if (!k.ease.is_linear())
            all_linear = false;
    }
    check(all_linear, "resampled keys are all linear");
    check(flat.keys().size() > track.keys().size(), "resampling adds keys");

    const double range = 4.0;
    bool within = true;
    for (int step = 0; step <= 40; ++step) {
        const double x = static_cast<double>(step) / 40.0;
        const double error = std::abs(flat.value_at(x, 0.0) - track.value_at(x, 0.0));
        if (error >= range * 0.005)
            within = false;
    }
    check(within, "resampled ride matches the curve");

    const KeyTrack straight({ key(0.0, 0.0, Ease::linear()), key(1.0, 1.0, Ease::linear()) });
    check(straight.resample(12) == straight, "linear ride is untouched");
}

// --- shader.rs -------------------------------------------------------------

void test_identity_table_returns_what_it_is_given()
{
    const Lut lut = Lut::identity(17);
    const std::array<std::array<float, 3>, 4> samples = { {
            { 0.0f, 0.0f, 0.0f },
            { 1.0f, 1.0f, 1.0f },
            { 0.25f, 0.5f, 0.75f },
            { 0.9f, 0.1f, 0.3f },
    } };
    for (const auto &rgb : samples) {
        const auto out = lut.sample(rgb);
        for (int i = 0; i < 3; ++i) {
            check_near(out[i], rgb[i], 0.01, "identity table round-trips");
        }
    }
}

void test_table_of_wrong_length_is_refused()
{
    check(!Lut::from_rgb(3, std::vector<float>(26 * 3, 0.0f)).has_value(),
          "wrong-length table refused");
    check(!Lut::from_rgb(1, std::vector<float>(3, 0.0f)).has_value(), "size below two refused");
    std::vector<float> broken(8 * 3, 0.5f);
    broken[4] = std::nanf("");
    check(!Lut::from_rgb(2, broken).has_value(), "NaN table refused");
}

void test_table_keeps_values_as_given()
{
    std::vector<float> rgb;
    for (int i = 0; i < 8; ++i) {
        rgb.push_back(0.1234567f);
        rgb.push_back(-0.25f);
        rgb.push_back(1.75f);
    }
    const auto lut = Lut::from_rgb(2, rgb);
    check(lut.has_value(), "table accepted");
    const auto out = lut->sample({ 0.3f, 0.6f, 0.9f });
    check_near(out[0], 0.1234567, 1e-6, "value kept past range");
    check_near(out[1], -0.25, 1e-6, "negative value kept");
    check_near(out[2], 1.75, 1e-6, "value above one kept");

    std::vector<float> other;
    for (int i = 0; i < 8; ++i) {
        other.push_back(0.1234568f);
        other.push_back(-0.25f);
        other.push_back(1.75f);
    }
    check(lut->id != Lut::from_rgb(2, other)->id, "id tracks contents");
}

void test_identity_reveal_map_reveals_everywhere()
{
    const RevealMap map = RevealMap::identity();
    bool all_zero = true;
    for (std::uint8_t g : *map.gray) {
        if (g != 0)
            all_zero = false;
    }
    check(all_zero, "identity reveal map is all zero");
}

void test_word_rects_are_ordered_and_outside_stays_revealed()
{
    const RevealMap map =
            RevealMap::from_rects(10, 10, { { 0, 0, 2, 2 }, { 5, 0, 2, 2 }, { 0, 5, 2, 2 } });
    check((*map.gray)[0] == 0, "first word is zero");
    check((*map.gray)[5] == 128, "second word is mid");
    check((*map.gray)[5 * 10] == 255, "third word is full");
    check((*map.gray)[9 * 10 + 9] == 0, "uncovered pixel stays revealed");
}

void test_stage_draws_layer_shrunk_and_rounded_up()
{
    const auto stage = [](std::array<std::uint32_t, 2> shrink) {
        Stage s;
        s.entry = "fs_small";
        s.shrink = shrink;
        return s;
    };
    check(stage({ 1, 1 }).size(1920, 1080) == std::make_pair(1920u, 1080u),
          "shrink one is the layer");
    check(stage({ 4, 1 }).size(1920, 1080) == std::make_pair(480u, 1080u), "horizontal shrink");
    check(stage({ 16, 16 }).size(1921, 1080) == std::make_pair(121u, 68u), "shrink rounds up");
    check(stage({ 64, 64 }).size(8, 8) == std::make_pair(1u, 1u), "never smaller than a pixel");
    check(stage({ 0, 2 }).size(10, 10) == std::make_pair(10u, 5u), "zero shrink is none");
}

void test_single_word_reveals_at_the_very_start()
{
    const RevealMap map = RevealMap::from_rects(4, 4, { { 0, 0, 2, 2 } });
    check((*map.gray)[0] == 0, "single word is zero");
}

} // namespace

int main()
{
    test_flat_curve_is_constant_speed();
    test_ends_anchored_and_area_exact();
    test_pieces_add_up_to_whole();
    test_nonsense_is_tidied_or_refused();

    test_black_is_opaque();
    test_transparent_is_clear();
    test_rejects_wrong_buffer_size();
    test_out_of_bounds_is_none();
    test_set_and_read_pixel();
    test_rows_are_tightly_packed();

    test_rational_survives_large_cross_products();

    test_track_holds_ends_and_eases_between();
    test_animation_is_relative_to_clip();
    test_eases_bend_the_way_their_names_say();
    test_resampling_follows_the_curve_it_flattens();

    test_identity_table_returns_what_it_is_given();
    test_table_of_wrong_length_is_refused();
    test_table_keeps_values_as_given();
    test_identity_reveal_map_reveals_everywhere();
    test_word_rects_are_ordered_and_outside_stays_revealed();
    test_stage_draws_layer_shrunk_and_rounded_up();
    test_single_word_reveals_at_the_very_start();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
