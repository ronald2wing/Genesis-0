// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-media/src/peaks.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// Waveform peaks: min/max sample pairs, bucketed at a fixed rate.
//
// The timeline draws a clip's waveform from a few hundred buckets per
// second, not from the samples themselves. The reduction folds decoded
// samples into buckets as they arrive and never accumulates them, so an
// hour-long recording costs the same memory as a jingle. The decode itself
// is the engine's business (R5): this module is pure arithmetic over the
// floats it is handed.

namespace genesis::render {

// A file's waveform, reduced to per-bucket extremes.
//
// Buckets are seeded at zero rather than ±infinity: silence reads as a
// flat 0/0 pair, and a bucket's minimum can never sit above the axis.
// That is the shape the timeline has always drawn, and the on-disk caches
// already hold it.
struct Peaks
{
    // The lowest sample in each bucket, in [-1, 0].
    std::vector<float> min;
    // The highest sample in each bucket, in [0, 1].
    std::vector<float> max;
    // How many buckets cover one second of audio.
    float buckets_per_second = 0.0f;

    bool operator==(const Peaks &) const = default;

    // The cache format:
    // `[buckets_per_second f32][count u32][min f32 x count][max f32 x count]`,
    // little-endian. Frozen: the caches beside existing projects hold it.
    std::vector<std::uint8_t> encode() const;

    // The inverse of `encode`: absent for bytes that do not have that shape,
    // so a corrupt cache entry regenerates instead of drawing.
    static std::optional<Peaks> decode(std::span<const std::uint8_t> bytes);

    // How many buckets there are.
    std::size_t len() const;

    // Whether there are none.
    bool is_empty() const;

    // The same waveform at `factor` buckets to one: each new bucket takes
    // the extremes of the buckets it folds, so nothing quieter or louder
    // than the fine shape appears at the coarse one. A trailing partial
    // group still counts.
    Peaks coarser(std::size_t factor) const;

    // The extremes over the buckets covering `from` to `to` seconds of the
    // file, `(low, high)` with low at or below zero and high at or above
    // it. Past the end, or an empty span, is silence.
    std::pair<float, float> extremes(float from, float to) const;
};

// A waveform at every resolution a drawing could want: the file's peaks
// as extracted, then halved again and again until a level is a handful of
// buckets. A drawing asks for the level whose buckets are about the size
// of its columns, so a clip drawn across two thousand pixels reads two
// thousand buckets and not two million, and a clip drawn across twenty
// reads twenty - the same extremes either way, since every level is the
// fold of the one beneath.
class Pyramid
{
public:
    // Every level, from `finest` down to a few buckets.
    static Pyramid of(Peaks finest);

    // The peaks as extracted.
    const Peaks &finest() const;

    // The level to draw a column of `seconds` from: the coarsest whose
    // buckets are no larger than the column, so a column reads one bucket
    // or a few and never a fraction of one. The finest when even it is
    // coarser than the column.
    const Peaks &level_for(float seconds_per_column) const;

    // How many levels there are.
    std::size_t depth() const;

private:
    // Finest first.
    std::vector<Peaks> levels;
};

// Reduces interleaved float samples to min/max buckets: the waveform the
// timeline draws. The decoder that produces `samples` is the engine's
// business (R5); this is pure arithmetic over the numbers it hands over.
//
// `samples` holds `channels` interleaved values per frame, in [-1, 1];
// each frame is downmixed to mono - its channels' mean, the mono decode the
// Rust source's `extract` asked for - before folding. `sample_rate` is the
// rate the caller decoded at; the engine adapter should decode at one fixed
// rate (the Rust source's PEAK_RATE) so bucket sizes are exact and the
// returned `buckets_per_second` matches the request. `buckets_per_second`
// is clamped to [1, sample_rate].
Peaks reduce_peaks(std::span<const float> samples, std::uint32_t channels,
                   std::uint32_t sample_rate, std::uint32_t buckets_per_second);

} // namespace genesis::render
