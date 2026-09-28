// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace genesis::adapters::engine::ges {

// A request to decode a media file's chosen audio stream into a raw PCM file
// the caption runtime reads: 16 kHz mono float32 little-endian, no header, 4
// bytes per sample - the exact format WhisperProvider::transcribe_file takes.
struct AudioExtractRequest
{
    std::filesystem::path source;
    std::filesystem::path destination;
    // Which audio stream to decode, in file order (the same indexing Probe
    // reports on NewMedia::audio_tracks). 0 is the first audio stream; a
    // larger index selects a later one.
    std::uint32_t stream_index = 0;
};

// Why an extraction failed. Distinct codes so the caller can react (retry,
// skip, or surface a message) without parsing text. No exception crosses the
// seam: every failure is a result with one of these set.
enum class AudioExtractError {
    None, // success
    NoAudioStream, // the file has no audio stream, or the index is out of range
    OpenOrDecode, // the source is missing, unreadable, or cannot be probed
    PipelineError, // the decode pipeline failed to build, stalled, or errored
};

// What an extraction reports back. On success `error` is None and `samples`
// holds the number of float32 samples written; on failure `error` is set and
// `message` carries a human-readable reason. A failed run never leaves a
// partial destination behind.
struct AudioExtractResult
{
    AudioExtractError error = AudioExtractError::None;
    std::string message;
    // Float32 samples written, so the caller can check the PCM span without
    // re-stating the file (each sample is 4 bytes). Zero on failure.
    std::uint64_t samples = 0;
    // The decoded span in seconds, from `samples` at the fixed 16 kHz rate.
    double duration_seconds = 0.0;
};

// Decodes the chosen audio stream of `request.source` to
// `request.destination` as raw 16 kHz mono float32 PCM. Blocking and
// time-bounded: run it off the UI thread (R4). The whole stream is decoded -
// the caption runtime needs the complete clip, not a sample of it.
AudioExtractResult extract_audio(const AudioExtractRequest &request);

} // namespace genesis::adapters::engine::ges
