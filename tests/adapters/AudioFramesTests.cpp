// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <gst/gst.h>

#include "adapters/engine/ges/media/AudioFrames.h"

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

namespace ges_adapter = genesis::adapters::engine::ges;

// The extractor's fixed format: 16 kHz mono float32, 4 bytes per sample.
constexpr std::uint64_t kBytesPerSample = sizeof(float);

// Runs gst-launch to synthesize a fixture and returns its path, or empty when
// generation fails (the test then fails loudly, matching the repo's other
// engine suites).
std::string synthesize(const std::string &command, const std::filesystem::path &path)
{
    std::filesystem::remove(path);
    const std::string full =
            command + " filesink location=\"" + path.string() + "\" >/dev/null 2>&1";
    const bool ok = std::system(full.c_str()) == 0 && std::filesystem::exists(path)
            && std::filesystem::file_size(path) > 0;
    if (!ok) {
        std::filesystem::remove(path);
        return { };
    }
    return path.string();
}

// A one-second WAV: 44100 samples of mono S16LE at 44.1 kHz (100 buffers of
// 441 samples). The extractor resamples this to exactly 16000 samples.
std::string wav_fixture()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-audio-extract.wav";
    return synthesize("gst-launch-1.0 -q audiotestsrc wave=sine samplesperbuffer=441 "
                      "num-buffers=100 ! audio/x-raw,rate=44100,channels=1,format=S16LE ! "
                      "wavenc !",
                      path);
}

// A video-only WebM: a picture and no sound, for the no-audio error path.
std::string video_only_fixture()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-silent.webm";
    return synthesize("gst-launch-1.0 -q videotestsrc num-buffers=30 ! "
                      "video/x-raw,width=640,height=360,framerate=30/1 ! videoconvert ! "
                      "vp8enc ! webmmux !",
                      path);
}

// A two-audio-stream Matroska whose streams differ in length: the first is a
// one-second sine, the second a two-second noise. Extracting each by index
// must yield a sample count that follows the chosen stream's duration, proving
// the index selects the right track rather than always the first.
std::string two_stream_fixture()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-two.mkv";
    return synthesize("gst-launch-1.0 -q "
                      "audiotestsrc wave=sine samplesperbuffer=441 num-buffers=100 ! "
                      "audio/x-raw,rate=44100,channels=1 ! audioconvert ! vorbisenc ! mux. "
                      "audiotestsrc wave=white-noise samplesperbuffer=441 num-buffers=200 ! "
                      "audio/x-raw,rate=44100,channels=1 ! audioconvert ! vorbisenc ! mux. "
                      "matroskamux name=mux !",
                      path);
}

// The number of float32 samples in a raw PCM file, as written.
std::uint64_t pcm_sample_count(const std::filesystem::path &path)
{
    std::error_code ec;
    const std::uintmax_t bytes = std::filesystem::file_size(path, ec);
    return ec ? 0 : bytes / kBytesPerSample;
}

// Extracts a one-second WAV and asserts the output is a correctly sized raw
// 16 kHz mono float32 file: whole samples, the count within a hair of 16000,
// and a finite float in the first sample (proof it is PCM, not a header).
void test_extracts_a_wav()
{
    const std::string source = wav_fixture();
    check(!source.empty(), "a WAV fixture is available");
    if (source.empty()) {
        return;
    }
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-out.pcm";
    std::filesystem::remove(destination);

    ges_adapter::AudioExtractRequest request;
    request.source = source;
    request.destination = destination;

    const ges_adapter::AudioExtractResult result = ges_adapter::extract_audio(request);
    check(result.error == ges_adapter::AudioExtractError::None, "extract_audio succeeds");
    if (result.error != ges_adapter::AudioExtractError::None) {
        std::printf("  extract_audio said: %s\n", result.message.c_str());
    }
    check(std::filesystem::exists(destination), "the PCM file exists");
    check(std::filesystem::file_size(destination) > 0, "the PCM file is non-empty");

    const std::uint64_t samples = pcm_sample_count(destination);
    // 44100 source samples at 16 kHz is exactly 16000, within a small
    // resampler-window tolerance.
    check(samples >= 16000 - 64 && samples <= 16000 + 64,
          "PCM holds ~16000 samples (got " + std::to_string(samples) + ")");
    check(result.samples == samples, "result.samples matches the bytes written");
    if (result.samples == samples) {
        check(std::fabs(result.duration_seconds - 1.0) <= 0.01,
              "duration_seconds is ~1.0 (got " + std::to_string(result.duration_seconds) + ")");
    }

    std::ifstream in(destination, std::ios::binary);
    float first = 0.0f;
    check(in.read(reinterpret_cast<char *>(&first), sizeof(first)) && std::isfinite(first),
          "the first sample is a finite float");

    std::filesystem::remove(destination);
    std::filesystem::remove(source);
}

// Extracts each stream of a two-stream file by index and asserts the sample
// count tracks the chosen stream's duration.
void test_extracts_by_index()
{
    const std::string source = two_stream_fixture();
    check(!source.empty(), "a two-stream fixture is available");
    if (source.empty()) {
        return;
    }

    const std::filesystem::path first =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-first.pcm";
    const std::filesystem::path second =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-second.pcm";

    ges_adapter::AudioExtractRequest request;
    request.source = source;

    request.stream_index = 0;
    request.destination = first;
    const ges_adapter::AudioExtractResult r0 = ges_adapter::extract_audio(request);
    check(r0.error == ges_adapter::AudioExtractError::None, "stream 0 extracts");
    const std::uint64_t first_samples = pcm_sample_count(first);

    request.stream_index = 1;
    request.destination = second;
    const ges_adapter::AudioExtractResult r1 = ges_adapter::extract_audio(request);
    check(r1.error == ges_adapter::AudioExtractError::None, "stream 1 extracts");
    const std::uint64_t second_samples = pcm_sample_count(second);

    // The second stream is twice as long, so it must yield roughly twice the
    // samples. The window is wide enough for vorbis priming yet far too tight
    // to confuse a one-second stream with a two-second one.
    check(second_samples > first_samples * 3 / 2,
          "stream 1 is longer than stream 0 (" + std::to_string(first_samples) + " vs "
                  + std::to_string(second_samples) + ")");

    std::filesystem::remove(first);
    std::filesystem::remove(second);
    std::filesystem::remove(source);
}

// A video-only file has no audio, so extraction reports NoAudioStream and
// writes nothing.
void test_no_audio_stream()
{
    const std::string source = video_only_fixture();
    check(!source.empty(), "a video-only fixture is available");
    if (source.empty()) {
        return;
    }
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-never-created.pcm";
    std::filesystem::remove(destination);

    ges_adapter::AudioExtractRequest request;
    request.source = source;
    request.destination = destination;

    const ges_adapter::AudioExtractResult result = ges_adapter::extract_audio(request);
    check(result.error == ges_adapter::AudioExtractError::NoAudioStream,
          "a video-only file reports NoAudioStream");
    check(!result.message.empty(), "the failure carries a message");
    check(!std::filesystem::exists(destination), "no destination is written");

    std::filesystem::remove(source);
}

// A missing source reports OpenOrDecode, distinct from a file with no audio.
void test_missing_source()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-missing.wav";
    std::filesystem::remove(missing);
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-never-created.pcm";
    std::filesystem::remove(destination);

    ges_adapter::AudioExtractRequest request;
    request.source = missing;
    request.destination = destination;

    const ges_adapter::AudioExtractResult result = ges_adapter::extract_audio(request);
    check(result.error == ges_adapter::AudioExtractError::OpenOrDecode,
          "a missing source reports OpenOrDecode");
    check(!std::filesystem::exists(destination), "a missing source creates no destination");
}

// A stream index past the file's last audio track is NoAudioStream, the same
// code as a file with no audio at all.
void test_index_out_of_range()
{
    const std::string source = wav_fixture();
    check(!source.empty(), "a WAV fixture is available");
    if (source.empty()) {
        return;
    }
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-audio-extract-never-created.pcm";
    std::filesystem::remove(destination);

    ges_adapter::AudioExtractRequest request;
    request.source = source;
    request.destination = destination;
    request.stream_index = 5;

    const ges_adapter::AudioExtractResult result = ges_adapter::extract_audio(request);
    check(result.error == ges_adapter::AudioExtractError::NoAudioStream,
          "an out-of-range stream index reports NoAudioStream");
    check(!std::filesystem::exists(destination), "an out-of-range index creates no destination");

    std::filesystem::remove(source);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);

    test_extracts_a_wav();
    test_extracts_by_index();
    test_no_audio_stream();
    test_missing_source();
    test_index_out_of_range();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
