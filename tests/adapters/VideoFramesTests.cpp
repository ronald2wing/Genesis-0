// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "adapters/engine/ges/media/VideoFrames.h"

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

// A 30-frame WebM at 30 fps, 640x360: `num-buffers=30` makes the frame count
// exact, so the extractor's delivery count is predictable.
std::string video_fixture()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-video-frames.webm";
    return synthesize("gst-launch-1.0 -q videotestsrc num-buffers=30 ! "
                      "video/x-raw,width=640,height=360,framerate=30/1 ! videoconvert ! "
                      "vp8enc ! webmmux !",
                      path);
}

// An audio-only WAV: no picture, for the no-video error path.
std::string audio_only_fixture()
{
    const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "genesis-video-frames.wav";
    return synthesize("gst-launch-1.0 -q audiotestsrc wave=sine samplesperbuffer=441 "
                      "num-buffers=100 ! audio/x-raw,rate=44100,channels=1,format=S16LE ! "
                      "wavenc !",
                      path);
}

// Extracts every frame of the fixture and asserts the delivered count,
// dimensions and frame indexes are exact, and that real RGB bytes flowed.
void test_extracts_every_frame()
{
    const std::string source = video_fixture();
    check(!source.empty(), "a video fixture is available");
    if (source.empty()) {
        return;
    }

    ges_adapter::VideoFramesRequest request;
    request.source = source;

    std::vector<std::uint32_t> indexes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool nonzero = false;
    const ges_adapter::VideoFramesResult result = ges_adapter::extract_video_frames(
            request, [&](const genesis::ai::vision::FrameView &frame, std::uint32_t index) {
                width = frame.width;
                height = frame.height;
                indexes.push_back(index);
                for (std::size_t i = 0;
                     i < static_cast<std::size_t>(frame.width) * frame.height * 3; ++i) {
                    if (frame.rgb[i] != 0) {
                        nonzero = true;
                    }
                }
                return true;
            });

    check(result.error == ges_adapter::VideoFramesError::None, "extract_video_frames succeeds");
    check(result.frames_delivered == 30,
          "all 30 frames are delivered (got " + std::to_string(result.frames_delivered) + ")");
    check(result.width == 640 && result.height == 360,
          "the frames are 640x360 (got " + std::to_string(result.width) + "x"
                  + std::to_string(result.height) + ")");
    check(indexes.size() == 30, "the callback ran 30 times");
    if (indexes.size() == 30) {
        check(indexes.front() == 0 && indexes.back() == 29, "frame indexes run 0..29");
    }
    check(nonzero, "the delivered RGB bytes are not all zero");

    std::filesystem::remove(source);
}

// Extracts every 5th frame and asserts the delivered count and the exact
// source-frame indexes the decimation selected.
void test_decimation_every_nth()
{
    const std::string source = video_fixture();
    check(!source.empty(), "a video fixture is available");
    if (source.empty()) {
        return;
    }

    ges_adapter::VideoFramesRequest request;
    request.source = source;
    request.every_nth = 5;

    std::vector<std::uint32_t> indexes;
    const ges_adapter::VideoFramesResult result = ges_adapter::extract_video_frames(
            request, [&](const genesis::ai::vision::FrameView &, std::uint32_t index) {
                indexes.push_back(index);
                return true;
            });

    check(result.error == ges_adapter::VideoFramesError::None, "decimated extraction succeeds");
    check(result.frames_delivered == 6,
          "every 5th frame of 30 delivers 6 frames (got " + std::to_string(result.frames_delivered)
                  + ")");
    if (indexes.size() == 6) {
        const std::vector<std::uint32_t> expected = { 0, 5, 10, 15, 20, 25 };
        check(indexes == expected, "the delivered indexes are 0,5,10,15,20,25");
    }

    std::filesystem::remove(source);
}

// A target fps of 6 against a 30 fps source is the same 5-frame step, proving
// the target_fps -> step derivation uses the probed rate.
void test_target_fps()
{
    const std::string source = video_fixture();
    check(!source.empty(), "a video fixture is available");
    if (source.empty()) {
        return;
    }

    ges_adapter::VideoFramesRequest request;
    request.source = source;
    request.target_fps = 6.0;

    const ges_adapter::VideoFramesResult result = ges_adapter::extract_video_frames(
            request, [](const genesis::ai::vision::FrameView &, std::uint32_t) { return true; });

    check(result.error == ges_adapter::VideoFramesError::None, "target-fps extraction succeeds");
    check(result.frames_delivered == 6,
          "6 fps from 30 fps delivers 6 frames (got " + std::to_string(result.frames_delivered)
                  + ")");

    std::filesystem::remove(source);
}

// A mid-decode abort reports Cancelled and stops before the stream ends.
void test_cancel()
{
    const std::string source = video_fixture();
    check(!source.empty(), "a video fixture is available");
    if (source.empty()) {
        return;
    }

    ges_adapter::VideoFramesRequest request;
    request.source = source;

    std::uint64_t delivered = 0;
    const ges_adapter::VideoFramesResult result = ges_adapter::extract_video_frames(
            request,
            [&](const genesis::ai::vision::FrameView &, std::uint32_t) {
                ++delivered;
                return true;
            },
            [&]() { return delivered >= 5; });

    check(result.error == ges_adapter::VideoFramesError::Cancelled,
          "a mid-decode abort reports Cancelled");
    check(result.frames_delivered == 5,
          "the abort stops after 5 frames (got " + std::to_string(result.frames_delivered) + ")");

    std::filesystem::remove(source);
}

// An audio-only file has no video, so extraction reports NoVideoStream and
// delivers nothing.
void test_no_video_stream()
{
    const std::string source = audio_only_fixture();
    check(!source.empty(), "an audio-only fixture is available");
    if (source.empty()) {
        return;
    }

    ges_adapter::VideoFramesRequest request;
    request.source = source;
    std::uint64_t called = 0;
    const ges_adapter::VideoFramesResult result = ges_adapter::extract_video_frames(
            request, [&](const genesis::ai::vision::FrameView &, std::uint32_t) {
                ++called;
                return true;
            });

    check(result.error == ges_adapter::VideoFramesError::NoVideoStream,
          "an audio-only file reports NoVideoStream");
    check(!result.message.empty(), "the failure carries a message");
    check(called == 0, "no frame is delivered");

    std::filesystem::remove(source);
}

// A missing source reports OpenOrDecode, distinct from a file with no video.
void test_missing_source()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-video-frames-missing.webm";
    std::filesystem::remove(missing);

    ges_adapter::VideoFramesRequest request;
    request.source = missing;
    const ges_adapter::VideoFramesResult result = ges_adapter::extract_video_frames(
            request, [](const genesis::ai::vision::FrameView &, std::uint32_t) { return true; });

    check(result.error == ges_adapter::VideoFramesError::OpenOrDecode,
          "a missing source reports OpenOrDecode");
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);

    test_extracts_every_frame();
    test_decimation_every_nth();
    test_target_fps();
    test_cancel();
    test_no_video_stream();
    test_missing_source();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
