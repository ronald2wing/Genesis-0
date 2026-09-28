// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "render/Peaks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace genesis::render {

namespace {

// Appends `value`'s bytes, little-endian, as the Rust `to_le_bytes` does.
// The hosts this targets are little-endian, so a plain `bit_cast` matches.
template <typename T>
void append_le(std::vector<std::uint8_t> &out, T value)
{
    const std::array<std::uint8_t, sizeof(T)> bytes =
            std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
    out.insert(out.end(), bytes.begin(), bytes.end());
}

// Reads `sizeof(T)` little-endian bytes back into a `T`; the inverse of
// `append_le`.
template <typename T>
T read_le(std::span<const std::uint8_t> bytes)
{
    std::array<std::uint8_t, sizeof(T)> raw{ };
    std::copy_n(bytes.begin(), sizeof(T), raw.begin());
    return std::bit_cast<T>(raw);
}

// Folds a sample stream into peaks without holding the samples.
//
// The Rust `Folder` folds at the fixed PEAK_RATE; here the rate is the one
// the caller decoded at, so the same clamp and integer bucket size hold for
// whatever rate the engine hands over.
struct Folder
{
    std::uint32_t sample_rate;
    std::size_t bucket_size;
    std::vector<float> min;
    std::vector<float> max;
    float low = 0.0F;
    float high = 0.0F;
    std::size_t filled = 0;

    Folder(std::uint32_t sample_rate, std::uint32_t buckets_per_second)
        : sample_rate(sample_rate),
          bucket_size(sample_rate / std::clamp(buckets_per_second, std::uint32_t{ 1 }, sample_rate))
    {
    }

    void fold(float sample)
    {
        low = std::min(low, sample);
        high = std::max(high, sample);
        ++filled;
        if (filled == bucket_size) {
            min.push_back(low);
            max.push_back(high);
            low = 0.0F;
            high = 0.0F;
            filled = 0;
        }
    }

    Peaks finish()
    {
        // The trailing partial bucket still counts - dropping it would shave
        // the last fraction of a second off every waveform.
        if (filled > 0) {
            min.push_back(low);
            max.push_back(high);
        }
        Peaks peaks;
        peaks.min = std::move(min);
        peaks.max = std::move(max);
        peaks.buckets_per_second =
                static_cast<float>(sample_rate) / static_cast<float>(bucket_size);
        return peaks;
    }
};

} // namespace

std::vector<std::uint8_t> Peaks::encode() const
{
    const std::size_t count = std::min(min.size(), max.size());
    std::vector<std::uint8_t> bytes;
    bytes.reserve(8 + (count * 8));
    append_le(bytes, buckets_per_second);
    append_le(bytes, static_cast<std::uint32_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        append_le(bytes, min[i]);
    }
    for (std::size_t i = 0; i < count; ++i) {
        append_le(bytes, max[i]);
    }
    return bytes;
}

std::optional<Peaks> Peaks::decode(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < 8) {
        return std::nullopt;
    }
    const float buckets_per_second = read_le<float>(bytes.subspan(0, 4));
    const std::uint32_t count = read_le<std::uint32_t>(bytes.subspan(4, 4));
    const std::size_t n = static_cast<std::size_t>(count);
    if (bytes.size() != 8 + (n * 8)) {
        return std::nullopt;
    }
    const auto floats = [&](std::size_t offset) {
        std::vector<float> out;
        out.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(read_le<float>(bytes.subspan(offset + (i * 4), 4)));
        }
        return out;
    };
    Peaks peaks;
    peaks.buckets_per_second = buckets_per_second;
    peaks.min = floats(8);
    peaks.max = floats(8 + (n * 4));
    return peaks;
}

std::size_t Peaks::len() const
{
    return std::min(min.size(), max.size());
}

bool Peaks::is_empty() const
{
    return len() == 0;
}

Peaks Peaks::coarser(std::size_t factor) const
{
    factor = std::max<std::size_t>(factor, 1);
    const std::size_t count = len();
    const std::size_t groups = (count + factor - 1) / factor;
    Peaks out;
    out.min.reserve(groups);
    out.max.reserve(groups);
    for (std::size_t group = 0; group < groups; ++group) {
        const std::size_t from = group * factor;
        const std::size_t to = std::min(from + factor, count);
        float low = 0.0F;
        float high = 0.0F;
        for (std::size_t i = from; i < to; ++i) {
            low = std::min(low, min[i]);
            high = std::max(high, max[i]);
        }
        out.min.push_back(low);
        out.max.push_back(high);
    }
    out.buckets_per_second = buckets_per_second / static_cast<float>(factor);
    return out;
}

std::pair<float, float> Peaks::extremes(float from, float to) const
{
    const std::size_t count = len();
    if (count == 0 || to <= from || buckets_per_second <= 0.0F) {
        return { 0.0F, 0.0F };
    }
    // The buckets that begin inside the span, so a bucket straddling
    // two columns is read by one of them and never both, with a hair of
    // float noise at a boundary forgiven. A span narrower than a bucket
    // reads the bucket it sits in.
    constexpr float HAIR = 1e-3F;
    const float start = from * buckets_per_second;
    const float end = to * buckets_per_second;
    std::size_t first = static_cast<std::size_t>(std::max(0.0F, std::ceil(start - HAIR)));
    first = std::min(first, count);
    std::size_t last = static_cast<std::size_t>(std::max(0.0F, std::ceil(end - HAIR)));
    last = std::min(last, count);
    if (first >= last) {
        if (start >= static_cast<float>(count)) {
            // Past the end is silence.
            return { 0.0F, 0.0F };
        }
        first = std::min(static_cast<std::size_t>(std::max(0.0F, std::floor(start))), count - 1);
        last = first + 1;
    }
    float low = 0.0F;
    float high = 0.0F;
    for (std::size_t i = first; i < last; ++i) {
        low = std::min(low, min[i]);
        high = std::max(high, max[i]);
    }
    return { low, high };
}

Pyramid Pyramid::of(Peaks finest)
{
    std::vector<Peaks> levels;
    levels.push_back(std::move(finest));
    while (!levels.empty() && levels.back().len() > 64) {
        levels.push_back(levels.back().coarser(2));
    }
    Pyramid pyramid;
    pyramid.levels = std::move(levels);
    return pyramid;
}

const Peaks &Pyramid::finest() const
{
    return levels[0];
}

const Peaks &Pyramid::level_for(float seconds_per_column) const
{
    if (std::isnan(seconds_per_column) || seconds_per_column <= 0.0F) {
        return finest();
    }
    for (auto it = levels.rbegin(); it != levels.rend(); ++it) {
        if (it->buckets_per_second * seconds_per_column >= 1.0F) {
            return *it;
        }
    }
    return finest();
}

std::size_t Pyramid::depth() const
{
    return levels.size();
}

Peaks reduce_peaks(std::span<const float> samples, std::uint32_t channels,
                   std::uint32_t sample_rate, std::uint32_t buckets_per_second)
{
    if (channels == 0 || sample_rate == 0) {
        return Peaks{ };
    }
    Folder folder(sample_rate, buckets_per_second);
    // Downmix each frame to mono and fold it, as the Rust `extract`'s mono
    // decode does before reduction. A frame cut short at the end of the
    // buffer is dropped rather than read past `samples`.
    for (std::size_t frame = 0; frame + channels <= samples.size(); frame += channels) {
        float mono = 0.0F;
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            mono += samples[frame + channel];
        }
        folder.fold(mono / static_cast<float>(channels));
    }
    return folder.finish();
}

} // namespace genesis::render
