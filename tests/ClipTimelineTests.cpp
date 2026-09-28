// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Test cases re-derived from Concat's concat-core test modules
// (time.rs, timeline.rs), GPL-3.0-or-later, Copyright (C) 2026 Jareer and
// Concat contributors.

#include "core/ClipTimeline.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace genesis::core;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool condition, const std::string &what)
{
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

void check_eq(std::int64_t actual, std::int64_t expected, const std::string &what)
{
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("FAIL: %s (got %lld, want %lld)\n", what.c_str(),
                    static_cast<long long>(actual), static_cast<long long>(expected));
    }
}

void check_eq(Rational actual, Rational expected, const std::string &what)
{
    ++g_checks;
    if (actual != expected) {
        ++g_failures;
        std::printf("FAIL: %s (got %s, want %s)\n", what.c_str(), actual.to_string().c_str(),
                    expected.to_string().c_str());
    }
}

void check_near(double actual, double expected, double tolerance, const std::string &what)
{
    ++g_checks;
    if (std::abs(actual - expected) > tolerance) {
        ++g_failures;
        std::printf("FAIL: %s (got %f, want %f)\n", what.c_str(), actual, expected);
    }
}

Rational seconds(std::int64_t value)
{
    return Rational::from_int(value);
}

// ---------------------------------------------------------------------------
// Rational
// ---------------------------------------------------------------------------

void test_rational_reduces_to_lowest_terms()
{
    const Rational r(6, 8);
    check_eq(r.numerator(), 3, "6/8 reduces to 3/4 (numerator)");
    check_eq(r.denominator(), 4, "6/8 reduces to 3/4 (denominator)");
}

void test_rational_normalises_negative_denominator()
{
    const Rational r(1, -2);
    check_eq(r.numerator(), -1, "1/-2 normalises to -1/2 (numerator)");
    check_eq(r.denominator(), 2, "1/-2 normalises to -1/2 (denominator)");
}

void test_rational_equal_values_compare_equal()
{
    check(Rational(1, 2) == Rational(50, 100), "1/2 equals 50/100");
}

void test_rational_arithmetic_is_exact()
{
    const Rational third(1, 3);
    check_eq(third + third + third, Rational::ONE, "1/3 + 1/3 + 1/3 is exactly 1");
}

void test_rational_floor_and_ceil_handle_negatives()
{
    check_eq(Rational(-3, 2).floor(), -2, "floor(-3/2) is -2");
    check_eq(Rational(-3, 2).ceil(), -1, "ceil(-3/2) is -1");
    check_eq(Rational(3, 2).floor(), 1, "floor(3/2) is 1");
    check_eq(Rational(3, 2).ceil(), 2, "ceil(3/2) is 2");
}

void test_rational_parses_ffmpeg_fractions()
{
    check(Rational::parse("30000/1001") == std::optional<Rational>(FrameRate::NTSC_30.fps()),
          "parses 30000/1001");
    check(Rational::parse("25") == std::optional<Rational>(seconds(25)), "parses a bare integer");
    check(Rational::parse("1.5") == std::optional<Rational>(Rational(3, 2)), "parses a decimal");
    check(Rational::parse("-1.25") == std::optional<Rational>(Rational(-5, 4)),
          "parses a negative decimal");
    check(!Rational::parse("0/0").has_value(), "rejects 0/0");
    check(!Rational::parse("N/A").has_value(), "rejects N/A");
}

void test_rational_approximate()
{
    check(Rational::approximate(0.5) == std::optional<Rational>(Rational(1, 2)),
          "approximates 0.5 to 1/2");
    check(!Rational::approximate(std::nan("")).has_value(), "rejects NaN");
    check(!Rational::approximate(INFINITY).has_value(), "rejects infinity");
}

void test_rational_survives_large_cross_products()
{
    // Cross products here exceed int64; the portable wide multiply must keep
    // the arithmetic exact rather than wrapping.
    const Rational tiny(1, 1'000'000'000);
    check_eq(tiny + tiny, Rational(1, 500'000'000), "1e-9 + 1e-9 is exact");
    check_eq(tiny * tiny, Rational(1, 1'000'000'000'000'000'000), "1e-9 * 1e-9 is exact");
    check(Rational(1, 3) < Rational(1, 2), "1/3 < 1/2 with large cross products");
    check(Rational(-1, 3) < Rational(1, 3), "-1/3 < 1/3");
    check(Rational(-1, 2) < Rational(-1, 3), "-1/2 < -1/3");
}

void test_ntsc_frame_time_is_exact()
{
    // Concat's doc example: frame 1800 of 29.97 fps is exactly 60.06 seconds.
    const Rational t = FrameRate::NTSC_30.time_of_frame(1800);
    check_eq(t, Rational(60060, 1000), "NTSC frame 1800 is 60060/1000");
    check_eq(FrameRate::NTSC_30.frame_at(t), 1800, "and it round-trips");
}

// ---------------------------------------------------------------------------
// FrameRate
// ---------------------------------------------------------------------------

void test_ntsc_never_drifts()
{
    // The whole reason this module exists: a double accumulator is visibly
    // wrong here, and this is exact after an hour of frames.
    const FrameRate rate = FrameRate::NTSC_30;
    Rational clock = Rational::ZERO;
    for (int i = 0; i < 107'892; ++i) {
        clock = clock + rate.frame_duration();
    }
    check_eq(clock, rate.time_of_frame(107'892), "an hour of NTSC frames accumulates exactly");
    check_eq(rate.frame_at(clock), 107'892, "and the frame index round-trips");
}

void test_frame_boundaries_belong_to_the_frame_that_starts_there()
{
    const FrameRate rate = FrameRate::THIRTY;
    check_eq(rate.frame_at(rate.time_of_frame(7)), 7, "a frame's own start maps back to it");
    check_eq(rate.frame_at(rate.time_of_frame(7) - Rational(1, 1'000'000)), 6,
             "an instant before belongs to the previous frame");
}

void test_frames_in()
{
    check_eq(FrameRate::THIRTY.frames_in(seconds(9)), 270, "9 seconds at 30 fps is 270 frames");
}

// ---------------------------------------------------------------------------
// TimeRange
// ---------------------------------------------------------------------------

void test_ranges_are_half_open()
{
    const TimeRange range(seconds(2), seconds(3));
    check(range.contains(seconds(2)), "the start is inside");
    check(!range.contains(seconds(5)), "the end is outside");
    check_eq(range.end(), seconds(5), "the end is start + duration");
}

void test_touching_ranges_do_not_overlap()
{
    const TimeRange a(Rational::ZERO, seconds(2));
    const TimeRange b(seconds(2), seconds(2));
    check(!a.overlaps(b), "touching ranges do not overlap");
    check(a.overlaps(TimeRange(Rational::ONE, seconds(2))), "overlapping ranges do overlap");
}

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

void test_arena_stale_handle_is_empty()
{
    Arena<int> arena;
    const ArenaId<int> id = arena.insert(7);
    check(arena.get(id) != nullptr && *arena.get(id) == 7, "a fresh handle reads back");
    arena.remove(id);
    check(arena.get(id) == nullptr, "a stale handle is empty");
}

void test_arena_reuses_slots_without_aliasing()
{
    Arena<int> arena;
    const ArenaId<int> first = arena.insert(1);
    arena.remove(first);
    const ArenaId<int> second = arena.insert(2);
    check(first != second, "a reused slot gets a new generation");
    check(arena.get(first) == nullptr, "the old handle stays empty");
    check(arena.get(second) != nullptr && *arena.get(second) == 2, "the new handle reads back");
}

// ---------------------------------------------------------------------------
// Clip
// ---------------------------------------------------------------------------

void test_clip_maps_timeline_time_to_source_time()
{
    Clip clip(MediaRef("a.mp4"), seconds(2), seconds(3));
    check(clip.source_time_at(seconds(2)) == std::optional<Rational>(Rational::ZERO),
          "the clip's start maps to source zero");
    check(clip.source_time_at(seconds(4)) == std::optional<Rational>(seconds(2)),
          "two seconds in maps to source two");
    check(!clip.source_time_at(seconds(1)).has_value(), "before the clip is nullopt");
    check(!clip.source_time_at(seconds(5)).has_value(), "the end is exclusive");
}

void test_speed_scales_the_source_time_map()
{
    Clip clip(MediaRef("a.mp4"), seconds(2), seconds(3));
    clip.set_speed(seconds(2));
    clip.set_source_start(seconds(10));

    // The clip covers timeline [2, 5) at 2x, so it consumes source [10, 16).
    check(clip.source_time_at(seconds(2)) == std::optional<Rational>(seconds(10)),
          "2x: start maps to source 10");
    check(clip.source_time_at(seconds(4)) == std::optional<Rational>(seconds(14)),
          "2x: two seconds in maps to source 14");
    check_eq(clip.source_duration(), seconds(6), "2x consumes 6 source seconds");

    clip.set_speed(Rational(1, 2));
    check(clip.source_time_at(seconds(4)) == std::optional<Rational>(seconds(11)),
          "half speed: two seconds in maps to source 11");
    check_eq(clip.source_duration(), Rational(3, 2), "half speed consumes 1.5 source seconds");
}

void test_in_point_offsets_the_source_time()
{
    Clip clip(MediaRef("a.mp4"), seconds(2), seconds(3));
    clip.set_source_start(seconds(10));
    check(clip.source_time_at(seconds(2)) == std::optional<Rational>(seconds(10)),
          "in-point offsets the start");
    check(clip.source_time_at(seconds(3)) == std::optional<Rational>(seconds(11)),
          "in-point offsets one second in");
}

void test_video_fades_ramp_and_hold()
{
    Clip clip(MediaRef("a.mp4"), seconds(2), seconds(3)); // covers [2, 5)
    clip.set_video_fade_in(Rational::ONE);
    clip.set_video_fade_out(Rational::ONE);

    check_near(clip.video_fade_factor(seconds(2)), 0.0, 1e-6, "starts at zero");
    check_near(clip.video_fade_factor(Rational(5, 2)), 0.5, 1e-6, "halfway up");
    check_near(clip.video_fade_factor(seconds(3)), 1.0, 1e-6, "holds at one between fades");
    check_near(clip.video_fade_factor(Rational(9, 2)), 0.5, 1e-6, "halfway down");
}

void test_no_fade_means_no_attenuation()
{
    const Clip clip(MediaRef("a.mp4"), seconds(2), seconds(3));
    check_near(clip.video_fade_factor(seconds(2)), 1.0, 1e-6,
               "no fade in means full opacity at the start");
    check_near(clip.video_fade_factor(seconds(4)), 1.0, 1e-6,
               "no fade out means full opacity at the end");
}

void test_fraction_at()
{
    const Clip clip(MediaRef("a.mp4"), seconds(2), seconds(4)); // [2, 6)
    check_near(clip.fraction_at(seconds(2)), 0.0, 1e-9, "fraction at start");
    check_near(clip.fraction_at(seconds(4)), 0.5, 1e-9, "fraction at middle");
    check_near(clip.fraction_at(seconds(6)), 1.0, 1e-9, "fraction at end");
    check_near(clip.fraction_at(seconds(1)), 0.0, 1e-9, "fraction before the clip clamps to zero");
}

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

void test_finds_the_clip_under_the_playhead()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    const auto clip = timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3)));
    check(clip.has_value(), "the clip is placed");

    check(timeline.clip_on_track_at(track, seconds(3)) == clip,
          "the clip is found under the playhead");
    check(!timeline.clip_on_track_at(track, seconds(9)).has_value(),
          "nothing is found past the clip");
}

// The per-track time-sorted index backs clip_on_track_at. These pin the exact
// query semantics the index must reproduce: the start is inclusive, the end is
// exclusive, a gap finds nothing, and on an overlap the last-added clip wins
// (matching what a paste shows) - all independent of insertion order.

void test_clip_on_track_at_boundaries()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    const auto clip =
            timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3))); // [2, 5)
    check(timeline.clip_on_track_at(track, seconds(2)) == clip, "the start is inside (inclusive)");
    check(!timeline.clip_on_track_at(track, seconds(5)).has_value(),
          "the end is outside (exclusive)");
}

void test_clip_on_track_at_gap_returns_none()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3)));
    timeline.add_clip(track, Clip(MediaRef("b.mp4"), seconds(7), seconds(3)));
    check(!timeline.clip_on_track_at(track, seconds(6)).has_value(),
          "a gap between clips finds nothing");
    check(!timeline.clip_on_track_at(track, seconds(1)).has_value(),
          "before the first clip finds nothing");
    check(!timeline.clip_on_track_at(track, seconds(11)).has_value(),
          "after the last clip finds nothing");
}

void test_clip_on_track_at_overlap_last_added_wins()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    const auto first =
            timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3))); // [2, 5)
    const auto second =
            timeline.add_clip(track, Clip(MediaRef("b.mp4"), seconds(4), seconds(3))); // [4, 7)
    check(first.has_value() && second.has_value(), "both clips are placed");

    check(timeline.clip_on_track_at(track, seconds(3)) == first,
          "before the overlap the earlier clip wins");
    check(timeline.clip_on_track_at(track, Rational(9, 2)) == second,
          "in the overlap the last-added clip wins");
    check(timeline.clip_on_track_at(track, seconds(6)) == second,
          "after the overlap the later clip alone remains");
}

void test_clip_on_track_at_same_start_last_added_wins()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    const auto first =
            timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3))); // [2, 5)
    const auto second =
            timeline.add_clip(track, Clip(MediaRef("b.mp4"), seconds(2), seconds(1))); // [2, 3)
    check(timeline.clip_on_track_at(track, Rational(5, 2)) == second,
          "with a shared start the last-added clip wins");
    check(timeline.clip_on_track_at(track, seconds(4)) == first,
          "past the shorter clip the longer earlier one remains");
}

void test_clip_on_track_at_remove_rebuilds_index()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    const auto first =
            timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3))); // [2, 5)
    const auto second =
            timeline.add_clip(track, Clip(MediaRef("b.mp4"), seconds(4), seconds(3))); // [4, 7)
    check(timeline.clip_on_track_at(track, Rational(9, 2)) == second,
          "the last-added clip wins before removal");

    check(timeline.remove_clip(*second).has_value(), "the later clip is removed");
    check(timeline.clip_on_track_at(track, Rational(9, 2)) == first,
          "after removal the earlier clip is found under the playhead");
    check(!timeline.clip_on_track_at(track, seconds(6)).has_value(),
          "past the earlier clip's end nothing remains");
}

void test_duration_is_the_end_of_the_last_clip()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3)));
    check_eq(timeline.duration(), seconds(5), "duration is the first clip's end");

    timeline.add_clip(track, Clip(MediaRef("b.mp4"), seconds(5), seconds(4)));
    check_eq(timeline.duration(), seconds(9), "duration follows the last clip");
    check_eq(timeline.frame_count(), 270, "9 seconds at 30 fps is 270 frames");
}

void test_tracks_come_back_bottom_most_first()
{
    Timeline timeline = Timeline::hd();
    const TrackId lower = timeline.add_track(Track("V1", TrackKind::Video));
    const TrackId upper = timeline.add_track(Track("V2", TrackKind::Video));
    check(timeline.track_ids().size() == 2, "two tracks");
    check(timeline.track_ids()[0] == lower, "the first track is bottom-most");
    check(timeline.track_ids()[1] == upper, "the second track is on top");
}

void test_removing_a_track_takes_its_clips_with_it()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    const auto clip = timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3)));
    timeline.remove_track(track);

    check(timeline.clip(*clip) == nullptr, "the clip is gone");
    check_eq(static_cast<std::int64_t>(timeline.clip_count()), 0, "no clips remain");
    check(timeline.track_ids().empty(), "no tracks remain");
    check_eq(timeline.duration(), Rational::ZERO, "duration is zero");
}

void test_removing_a_clip_unlinks_it_from_its_track()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    const auto clip = timeline.add_clip(track, Clip(MediaRef("a.mp4"), seconds(2), seconds(3)));

    check(timeline.remove_clip(*clip).has_value(), "the clip is removed");
    check(timeline.track(track)->clips().empty(), "the track is unlinked");
    check(!timeline.clip_on_track_at(track, seconds(3)).has_value(),
          "nothing is found under the playhead");
    check(!timeline.remove_clip(*clip).has_value(), "removing twice is harmless");
}

void test_a_stale_track_handle_does_not_leak_a_clip()
{
    Timeline timeline = Timeline::hd();
    const TrackId track = timeline.add_track(Track("V1", TrackKind::Video));
    timeline.remove_track(track);

    const auto orphan = timeline.add_clip(track, Clip(MediaRef("c.mp4"), seconds(0), seconds(1)));
    check(!orphan.has_value(), "a stale track handle refuses the clip");
    check_eq(static_cast<std::int64_t>(timeline.clip_count()), 0, "no orphaned clip is leaked");
}

void test_blend_names_round_trip()
{
    check(blend_name(Blend::Multiply) == "multiply", "multiply names itself");
    check(blend_parse("multiply") == Blend::Multiply, "multiply parses back");
    check(blend_parse("nonsense") == Blend::Normal, "an unknown name is Normal");
}

} // namespace

int main()
{
    test_rational_reduces_to_lowest_terms();
    test_rational_normalises_negative_denominator();
    test_rational_equal_values_compare_equal();
    test_rational_arithmetic_is_exact();
    test_rational_floor_and_ceil_handle_negatives();
    test_rational_parses_ffmpeg_fractions();
    test_rational_approximate();
    test_rational_survives_large_cross_products();
    test_ntsc_frame_time_is_exact();

    test_ntsc_never_drifts();
    test_frame_boundaries_belong_to_the_frame_that_starts_there();
    test_frames_in();

    test_ranges_are_half_open();
    test_touching_ranges_do_not_overlap();

    test_arena_stale_handle_is_empty();
    test_arena_reuses_slots_without_aliasing();

    test_clip_maps_timeline_time_to_source_time();
    test_speed_scales_the_source_time_map();
    test_in_point_offsets_the_source_time();
    test_video_fades_ramp_and_hold();
    test_no_fade_means_no_attenuation();
    test_fraction_at();

    test_finds_the_clip_under_the_playhead();
    test_clip_on_track_at_boundaries();
    test_clip_on_track_at_gap_returns_none();
    test_clip_on_track_at_overlap_last_added_wins();
    test_clip_on_track_at_same_start_last_added_wins();
    test_clip_on_track_at_remove_rebuilds_index();
    test_duration_is_the_end_of_the_last_clip();
    test_tracks_come_back_bottom_most_first();
    test_removing_a_track_takes_its_clips_with_it();
    test_removing_a_clip_unlinks_it_from_its_track();
    test_a_stale_track_handle_does_not_leak_a_clip();
    test_blend_names_round_trip();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
