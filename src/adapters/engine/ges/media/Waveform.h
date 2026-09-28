// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>

namespace genesis::adapters::engine::ges {

// The buckets per second a waveform cache folds at. A few hundred bars per
// second of audio, so an hour of sound costs the same reduction memory as a
// jingle while a card still reads every transient - and the bin's card
// coarsens it to its own handful of bars when it draws. This is also the
// CacheKey's `buckets_per_second`, so it is frozen with the cache: a change
// orphans every .peaks already written.
inline constexpr std::uint32_t kWaveformBucketsPerSecond = 200;

// A request to decode a source's chosen audio stream and fold it into the
// .peaks cache the bin's audio cards draw. Decoding is the engine's business
// (R5); the reduction is the host's (render::reduce_peaks).
struct WaveformRequest
{
    std::filesystem::path source;
    std::filesystem::path destination;
    // Which audio stream to decode, in file order (the same indexing Probe
    // reports on NewMedia::audio_tracks). 0 is the first audio stream.
    std::uint32_t stream_index = 0;
};

// Decodes the chosen audio stream to raw PCM (reusing AudioFrames' 16 kHz
// mono float32 format), folds it into min/max buckets at
// kWaveformBucketsPerSecond, and writes the .peaks cache (Peaks::encode's
// frozen layout) atomically at `destination`. Returns the path, or nullopt on
// a missing/unreadable source, a file with no audio, or a failed decode - a
// failed run never leaves a partial destination behind. Blocking and
// time-bounded: run it off the UI thread (R4).
std::optional<std::filesystem::path> make_waveform(const WaveformRequest &request);

} // namespace genesis::adapters::engine::ges
