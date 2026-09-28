// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/Waveform.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <system_error>
#include <vector>

#include "adapters/engine/ges/media/AudioFrames.h"
#include "render/Peaks.h"

namespace genesis::adapters::engine::ges {

namespace {

namespace render = genesis::render;

// The sample rate extract_audio resamples to: the fixed 16 kHz mono float32
// format the caption runtime reads. It is not returned by the extraction
// result, so it is restated here - the two must stay in lockstep, and a change
// to one is a change to the other.
constexpr std::uint32_t kSampleRate = 16000;

// Writes `bytes` to `destination` atomically: stage beside it (same
// filesystem) and rename, so a reader never observes a half-written .peaks.
// The same shape the model and mask stores use for their data files.
bool write_atomic(const std::filesystem::path &destination, const std::vector<std::uint8_t> &bytes)
{
    std::error_code ec;
    const std::filesystem::path parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }

    const std::filesystem::path staging = std::filesystem::path(destination.string() + ".tmp");
    {
        std::ofstream out(staging, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::error_code ignored;
            std::filesystem::remove(staging, ignored);
            return false;
        }
    }

    std::filesystem::remove(destination, ec);
    ec.clear();
    std::filesystem::rename(staging, destination, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        return false;
    }
    return true;
}

} // namespace

std::optional<std::filesystem::path> make_waveform(const WaveformRequest &request)
{
    // Decode the chosen stream to a raw PCM temp file - the exact 16 kHz mono
    // float32 format extract_audio produces - then fold it and write the
    // .peaks. The PCM never survives the call; only the reduced cache does.
    const std::filesystem::path pcm =
            std::filesystem::temp_directory_path() / "genesis-waveform.pcm";
    std::error_code ec;
    std::filesystem::remove(pcm, ec);

    const AudioExtractResult extracted =
            extract_audio(AudioExtractRequest{ request.source, pcm, request.stream_index });
    if (extracted.error != AudioExtractError::None) {
        std::filesystem::remove(pcm, ec);
        return std::nullopt;
    }

    // Read the PCM back whole: the reduction is pure arithmetic over the
    // floats, and the whole stream is needed to fold it, so it is loaded once
    // rather than re-read bucket by bucket.
    std::vector<float> samples(static_cast<std::size_t>(extracted.samples));
    {
        std::ifstream in(pcm, std::ios::binary);
        const bool ok = in
                && in.read(reinterpret_cast<char *>(samples.data()),
                           static_cast<std::streamsize>(samples.size() * sizeof(float)));
        std::filesystem::remove(pcm, ec);
        if (!ok) {
            return std::nullopt;
        }
    }

    const render::Peaks peaks =
            render::reduce_peaks(samples, 1, kSampleRate, kWaveformBucketsPerSecond);
    if (!write_atomic(request.destination, peaks.encode())) {
        return std::nullopt;
    }
    return request.destination;
}

} // namespace genesis::adapters::engine::ges
