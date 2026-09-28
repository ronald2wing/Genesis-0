// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "../support/Checks.h"
#include "render/Peaks.h"

namespace {

using genesis::test::check;
namespace render = genesis::render;

// The rate the Rust `extract` resampled to before folding: a fixed rate
// keeps the bucket size exact.
constexpr std::uint32_t kRate = 48000;

// Folds mono float samples at `kRate`, as the Rust `extract` did before
// reduction. The Rust inline tests fed i16 and divided by 32768; the
// equivalent here is floats already in [-1, 1].
render::Peaks fold(const std::vector<float> &samples, std::uint32_t buckets_per_second)
{
    return render::reduce_peaks(samples, 1, kRate, buckets_per_second);
}

// Reads a little-endian f32 out of `bytes` at `offset`, independent of the
// library's own decoder, so the cache layout is checked from the outside.
float le_f32(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    const std::uint32_t raw = static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    return std::bit_cast<float>(raw);
}

std::uint32_t le_u32(const std::vector<std::uint8_t> &bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

// buckets_carry_the_extremes_and_the_tail_partial_counts
void test_buckets_carry_the_extremes_and_the_tail_partial_counts()
{
    // 240 samples per bucket at 200 buckets/second: one full bucket
    // holding the extremes, then a 10-sample partial.
    std::vector<float> samples(240, 0.0f);
    samples[7] = -1.0f; // i16::MIN / 32768
    samples[100] = 0.5f; // 16384 / 32768
    samples.insert(samples.end(), 10, -0.25f); // -8192 / 32768

    const render::Peaks peaks = fold(samples, 200);
    check(peaks.buckets_per_second == 200.0f, "the requested rate");
    check(peaks.min.size() == 2, "two buckets");
    check(peaks.min[0] == -1.0f, "the floor carries through");
    check(peaks.max[0] == 0.5f, "the ceiling carries through");
    check(peaks.min[1] == -0.25f, "the tail partial counts");
    // Seeded at zero: an all-negative bucket still reports max 0.
    check(peaks.max[1] == 0.0f, "an all-negative bucket reports max 0");
}

// silence_is_flat_zeroes
void test_silence_is_flat_zeroes()
{
    const render::Peaks peaks = fold(std::vector<float>(480, 0.0f), 200);
    check(peaks.min == std::vector<float>({ 0.0f, 0.0f }), "min is flat zeroes");
    check(peaks.max == std::vector<float>({ 0.0f, 0.0f }), "max is flat zeroes");
}

// encode_lays_out_rate_count_min_max_and_decode_reads_it_back
void test_encode_lays_out_rate_count_min_max_and_decode_reads_it_back()
{
    render::Peaks peaks;
    peaks.min = { -0.5f, 0.0f };
    peaks.max = { 0.25f, 0.0f };
    peaks.buckets_per_second = 200.0f;

    const std::vector<std::uint8_t> bytes = peaks.encode();
    check(bytes.size() == 8 + 2 * 8, "the cache size");
    check(le_f32(bytes, 0) == 200.0f, "the rate is first");
    check(le_u32(bytes, 4) == 2, "the count is next");
    check(le_f32(bytes, 8) == -0.5f, "the first minimum");
    check(le_f32(bytes, 16) == 0.25f, "the first maximum");

    const std::optional<render::Peaks> back = render::Peaks::decode(bytes);
    check(back.has_value(), "decodes");
    check(back->min == peaks.min, "min round-trips");
    check(back->max == peaks.max, "max round-trips");
    check(back->buckets_per_second == 200.0f, "rate round-trips");
    check(!render::Peaks::decode(std::span<const std::uint8_t>(bytes).first(10)).has_value(),
          "a truncated entry does not decode");
}

// a_coarser_level_keeps_the_extremes_and_the_tail
void test_a_coarser_level_keeps_the_extremes_and_the_tail()
{
    render::Peaks fine;
    fine.min = { -0.1f, -0.9f, -0.2f, -0.3f, -0.5f };
    fine.max = { 0.4f, 0.2f, 0.8f, 0.1f, 0.6f };
    fine.buckets_per_second = 1000.0f;

    const render::Peaks half = fine.coarser(2);
    check(half.min == std::vector<float>({ -0.9f, -0.3f, -0.5f }), "min halves by extremes");
    check(half.max == std::vector<float>({ 0.4f, 0.8f, 0.6f }), "max halves by extremes");
    check(half.buckets_per_second == 500.0f, "half the rate");

    check(fine.coarser(0).len() == 5, "a factor of nothing is the same shape");

    render::Peaks empty;
    empty.buckets_per_second = 1000.0f;
    check(empty.coarser(4).is_empty(), "an empty fine level stays empty");
}

// the_pyramid_hands_out_the_level_that_fits_the_column
void test_the_pyramid_hands_out_the_level_that_fits_the_column()
{
    render::Peaks fine;
    fine.min = std::vector<float>(4096, -0.5f);
    fine.max = std::vector<float>(4096, 0.5f);
    fine.buckets_per_second = 1000.0f;

    const render::Pyramid pyramid = render::Pyramid::of(std::move(fine));
    check(pyramid.depth() == 7, "4096 halves to 64 in six steps");

    // A column of one millisecond reads the finest; of a second, the
    // coarsest that still puts at least one bucket in it.
    check(pyramid.level_for(0.001f).buckets_per_second == 1000.0f,
          "a millisecond column reads the finest");
    const render::Peaks &coarse = pyramid.level_for(1.0f);
    check(coarse.buckets_per_second <= 1000.0f / 64.0f + 1e-6f,
          "a second column reads near the coarsest");
    check(coarse.buckets_per_second * 1.0f >= 1.0f,
          "the coarse level still has a bucket per column");
    // A column finer than the finest bucket still gets the finest.
    check(pyramid.level_for(1e-9f).buckets_per_second == 1000.0f,
          "a nanoscopic column reads the finest");
    check(pyramid.level_for(std::nanf("")).buckets_per_second == 1000.0f,
          "a NaN column reads the finest");
    check(pyramid.level_for(-1.0f).buckets_per_second == 1000.0f,
          "a negative column reads the finest");

    // Every level says the same about the whole file.
    const auto [low, high] = pyramid.finest().extremes(0.0f, 4.096f);
    const auto [clow, chigh] = coarse.extremes(0.0f, 4.096f);
    check(low == clow && high == chigh, "every level agrees on the whole file");
    // Past the end is silence, not a panic.
    const auto [plow, phigh] = coarse.extremes(100.0f, 200.0f);
    check(plow == 0.0f && phigh == 0.0f, "past the end is silence");
    const auto [rlow, rhigh] = coarse.extremes(2.0f, 1.0f);
    check(rlow == 0.0f && rhigh == 0.0f, "an empty span is silence");
}

// A full-scale square wave pins every bucket to ±1.
void test_a_full_scale_square_reads_one_either_way()
{
    std::vector<float> samples;
    samples.reserve(480);
    for (std::size_t i = 0; i < 480; ++i) {
        samples.push_back((i % 2 == 0) ? 1.0f : -1.0f);
    }
    const render::Peaks peaks = fold(samples, 200);
    check(peaks.min == std::vector<float>({ -1.0f, -1.0f }), "every bucket floors at -1");
    check(peaks.max == std::vector<float>({ 1.0f, 1.0f }), "every bucket ceilings at +1");
}

// The bucket count tracks duration x buckets_per_second within one bucket.
void test_bucket_count_tracks_duration_times_rate()
{
    const std::vector<float> samples(1000, 0.0f);
    const render::Peaks peaks = fold(samples, 200);
    const double expected = static_cast<double>(samples.size()) / kRate * 200;
    check(peaks.min.size() == peaks.max.size(), "min and max agree on count");
    const double got = static_cast<double>(peaks.len());
    check(got >= expected - 1.0 && got <= expected + 1.0,
          "bucket count is within one of duration x rate");
}

// A stereo buffer is downmixed to mono before folding: out-of-phase channels
// cancel to silence, so the mix reads flat.
void test_stereo_downmix_folds_both_channels()
{
    std::vector<float> samples(480, 0.0f);
    for (std::size_t i = 0; i < 480; i += 2) {
        samples[i] = 1.0f;
        samples[i + 1] = -1.0f;
    }
    // 480 stereo floats fold to 240 mono samples: one full bucket.
    const render::Peaks peaks = render::reduce_peaks(samples, 2, kRate, 200);
    check(peaks.min == std::vector<float>({ 0.0f }), "out-of-phase stereo reads flat minima");
    check(peaks.max == std::vector<float>({ 0.0f }), "out-of-phase stereo reads flat maxima");
}

} // namespace

int main()
{
    test_buckets_carry_the_extremes_and_the_tail_partial_counts();
    test_silence_is_flat_zeroes();
    test_encode_lays_out_rate_count_min_max_and_decode_reads_it_back();
    test_a_coarser_level_keeps_the_extremes_and_the_tail();
    test_the_pyramid_hands_out_the_level_that_fits_the_column();
    test_a_full_scale_square_reads_one_either_way();
    test_bucket_count_tracks_duration_times_rate();
    test_stereo_downmix_folds_both_channels();

    return genesis::test::summary();
}
