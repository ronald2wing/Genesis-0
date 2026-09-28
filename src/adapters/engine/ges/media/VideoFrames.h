// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "ai/vision/SegmentationProvider.h"

namespace genesis::adapters::engine::ges {

// A request to decode a media file's video stream and hand its frames, one by
// one, to a callback. The frames are packed RGB8 at the source's own
// resolution - exactly the ai::vision::FrameView the segmentation provider
// takes, so a mask run decodes and segments without a copy or a resize in
// between. Mirrors AudioExtractRequest's shape; the deliverable difference is
// that frames are streamed to a callback rather than written to a file, so a
// long clip never has to be held whole in memory.
struct VideoFramesRequest
{
    std::filesystem::path source;
    // Which video stream to decode, in file order. 0 is the first video
    // stream; a larger index selects a later one (multi-video containers are
    // rare but not impossible).
    std::uint32_t stream_index = 0;
    // Deliver every Nth decoded source frame (1 = every frame). When
    // target_fps is positive it overrides this, derived from the probed
    // source rate; keep target_fps 0 to honour every_nth directly.
    std::uint32_t every_nth = 1;
    // A sampling rate in frames per second. A mask run does not need 30 fps -
    // 6 to 10 fps of alpha masks is enough - so this steps the source frames
    // by round(source_fps / target_fps). 0 = honour every_nth.
    double target_fps = 0.0;
};

// Why an extraction failed. Distinct codes so the caller can react (retry,
// skip, or surface a message) without parsing text. No exception crosses the
// seam: every failure is a result with one of these set.
enum class VideoFramesError {
    None, // success (or a clean stop with zero delivered frames)
    NoVideoStream, // the file has no video stream, or the index is out of range
    OpenOrDecode, // the source is missing, unreadable, or cannot be probed
    PipelineError, // the decode pipeline failed to build, stalled, or errored
    Cancelled, // the caller asked to stop mid-decode
};

// What an extraction reports back. On success `error` is None and
// `frames_delivered` counts the frames handed to the callback; on failure
// `error` is set and `message` carries a human-readable reason. `width` and
// `height` are the delivered frame dimensions (zero when nothing was
// delivered).
struct VideoFramesResult
{
    VideoFramesError error = VideoFramesError::None;
    std::string message;
    // Frames handed to the callback. The callback stopping early is a clean
    // stop, not a failure, so this counts exactly what was delivered.
    std::uint64_t frames_delivered = 0;
    // The delivered frame size, from the first frame (RGB8, width*height*3
    // bytes, stride = width*3). Zero when no frame was delivered.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// Called once per delivered frame. `frame` is packed RGB8, row-major, three
// bytes per pixel with no padding; it is valid only for the duration of the
// call. `frame_index` is the 0-based index of the source frame the bytes came
// from (so a decimated run still keys a mask to its true source frame).
// Returning false stops the decode cleanly (the result is None with the frames
// delivered so far); returning true keeps going.
using VideoFrameCallback =
        std::function<bool(const genesis::ai::vision::FrameView &frame, std::uint32_t frame_index)>;

// Decodes the chosen video stream of `request.source` and calls `callback`
// for every delivered frame. Blocking and time-bounded: run it off the UI
// thread (R4). `abort` is polled between frames; a true answer stops the run
// and reports Cancelled.
VideoFramesResult extract_video_frames(const VideoFramesRequest &request,
                                       const VideoFrameCallback &callback,
                                       const std::function<bool()> &abort = { });

} // namespace genesis::adapters::engine::ges
