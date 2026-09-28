// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The waveform peaks generator's suite: drives the real decode -> fold -> write
// pipeline headlessly. A synthesized WAV proves the .peaks cache parses back
// through the reader with the expected bucket count and rate; a video-only file
// and a missing source prove the no-audio and open/decode refusals write
// nothing.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "adapters/engine/ges/media/Waveform.h"
#include "render/Peaks.h"
#include "../support/Checks.h"

namespace {

using genesis::test::check;

namespace ges_adapter = genesis::adapters::engine::ges;
namespace render = genesis::render;

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

// A one-second WAV: 44100 samples of mono S16LE at 44.1 kHz. The extractor
// resamples this to ~16000 samples, which the generator folds into ~200
// buckets at 200 buckets/second.
std::string wav_fixture()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-waveform-fixture.wav";
    return synthesize("gst-launch-1.0 -q audiotestsrc wave=sine samplesperbuffer=441 "
                      "num-buffers=100 ! audio/x-raw,rate=44100,channels=1,format=S16LE ! "
                      "wavenc !",
                      path);
}

// A video-only WebM: a picture and no sound, for the no-audio refusal.
std::string video_only_fixture()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-waveform-fixture-silent.webm";
    return synthesize("gst-launch-1.0 -q videotestsrc num-buffers=30 ! "
                      "video/x-raw,width=640,height=360,framerate=30/1 ! videoconvert ! "
                      "vp8enc ! webmmux !",
                      path);
}

// Decodes a WAV and asserts the .peaks cache parses back with the expected
// rate and a bucket count that tracks the one-second span.
void test_waveform_folds_a_wav()
{
    const std::string source = wav_fixture();
    check(!source.empty(), "a WAV fixture is available");
    if (source.empty()) {
        return;
    }
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-waveform-fixture.peaks";
    std::filesystem::remove(destination);

    ges_adapter::WaveformRequest request;
    request.source = source;
    request.destination = destination;

    const auto made = ges_adapter::make_waveform(request);
    check(made.has_value(), "make_waveform succeeds");
    check(std::filesystem::exists(destination) && std::filesystem::file_size(destination) > 0,
          "the .peaks cache is written");

    std::ifstream in(destination, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                          std::istreambuf_iterator<char>());
    const auto decoded = render::Peaks::decode(bytes);
    check(decoded.has_value(), "the cache decodes through the reader");
    if (!decoded) {
        std::filesystem::remove(destination);
        std::filesystem::remove(source);
        return;
    }
    check(decoded->buckets_per_second == 200.0f, "the cache records 200 buckets/second");
    // 16000 samples at 80 samples per bucket is exactly 200 buckets, within a
    // hair for the resampler and the trailing partial bucket.
    check(decoded->len() >= 195 && decoded->len() <= 205,
          "the bucket count tracks the one-second span (got " + std::to_string(decoded->len())
                  + ")");
    // A sine is not silence: some bucket floors below zero and some ceilings
    // above it.
    check(*std::min_element(decoded->min.begin(), decoded->min.end()) < 0.0f,
          "the waveform has a negative minimum");
    check(*std::max_element(decoded->max.begin(), decoded->max.end()) > 0.0f,
          "the waveform has a positive maximum");

    std::filesystem::remove(destination);
    std::filesystem::remove(source);
}

// A video-only file has no audio, so make_waveform refuses and writes nothing.
void test_no_audio_stream()
{
    const std::string source = video_only_fixture();
    check(!source.empty(), "a video-only fixture is available");
    if (source.empty()) {
        return;
    }
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-waveform-never-created.peaks";
    std::filesystem::remove(destination);

    ges_adapter::WaveformRequest request;
    request.source = source;
    request.destination = destination;

    const auto made = ges_adapter::make_waveform(request);
    check(!made.has_value(), "a video-only file makes no waveform");
    check(!std::filesystem::exists(destination), "no destination is written");

    std::filesystem::remove(source);
}

// A missing source fails before the destination is touched.
void test_missing_source()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-waveform-missing.wav";
    std::filesystem::remove(missing);
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-waveform-never-created.peaks";
    std::filesystem::remove(destination);

    ges_adapter::WaveformRequest request;
    request.source = missing;
    request.destination = destination;

    const auto made = ges_adapter::make_waveform(request);
    check(!made.has_value(), "a missing source makes no waveform");
    check(!std::filesystem::exists(destination), "a missing source creates no destination");
}

} // namespace

int main(int argc, char **argv)
{
    gst_init(&argc, &argv);

    test_waveform_folds_a_wav();
    test_no_audio_stream();
    test_missing_source();

    return genesis::test::summary();
}
