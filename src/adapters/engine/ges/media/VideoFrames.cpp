// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/VideoFrames.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video-frame.h>
#include <gst/video/video-info.h>

#include "adapters/engine/ges/media/Probe.h"
#include "project/command/MediaCommands.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::ai::vision::FrameView;
using genesis::project::NewMedia;

// Wall-clock bound on the decode. A raw decode has no live element and so runs
// far faster than real time; this only guards against a wedged pipeline (a
// dead decoder, a stalled sink), never a long-but-healthy file.
constexpr guint64 kDecodeTimeoutUs = 300ull * 1000ull * 1000ull;

// How the decode finished.
enum class DecodeEnd { Eos, Error, Timeout, Cancelled };

// Whether a decodebin pad carries video.
bool pad_is_video(GstPad *pad)
{
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    if (caps == nullptr) {
        return false;
    }
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    const gchar *name = structure == nullptr ? nullptr : gst_structure_get_name(structure);
    const bool video = name != nullptr && g_str_has_prefix(name, "video/");
    gst_caps_unref(caps);
    return video;
}

// The state the pad-added handler drives: it keeps the chosen video stream
// (the target-th one, in file order) and routes every other stream to a fresh
// fakesink. A decodebin exposes a pad per stream, and a stream whose pad is
// left unlinked stalls or posts a not-linked error, so unselected streams must
// be consumed, not ignored - the same shape AudioFrames uses.
struct PadSelect
{
    GstElement *pipeline = nullptr;
    GstElement *convert = nullptr;
    std::uint32_t target = 0; // which video stream (file order) to keep
    std::uint32_t seen = 0; // video streams met so far
    bool linked = false; // the target pad is already wired
};

void on_pad_added(GstElement *, GstPad *pad, gpointer user_data)
{
    auto *select = static_cast<PadSelect *>(user_data);

    const bool video = pad_is_video(pad);
    if (video && select->seen == select->target && !select->linked) {
        ++select->seen;
        GstPad *sink = gst_element_get_static_pad(select->convert, "sink");
        if (sink != nullptr) {
            if (!gst_pad_is_linked(sink)) {
                gst_pad_link(pad, sink);
            }
            gst_object_unref(sink);
        }
        select->linked = true;
        return;
    }
    if (video) {
        ++select->seen;
    }

    // Any other stream (audio, subtitles, an unselected video track) is
    // consumed by its own fakesink so it cannot stall the decode.
    GstElement *drop = gst_element_factory_make("fakesink", nullptr);
    if (drop == nullptr) {
        return;
    }
    g_object_set(drop, "sync", FALSE, nullptr);
    gst_bin_add(GST_BIN(select->pipeline), drop);
    gst_element_sync_state_with_parent(drop);
    GstPad *sink = gst_element_get_static_pad(drop, "sink");
    if (sink != nullptr) {
        gst_pad_link(pad, sink);
        gst_object_unref(sink);
    }
}

// The step between delivered frames, from the request and the probed rate.
// target_fps wins over every_nth when positive: the step is
// round(source_fps / target_fps), clamped to at least 1. A source without a
// known rate falls back to every_nth.
std::uint32_t decimation_step(const VideoFramesRequest &request, const NewMedia &media)
{
    if (request.target_fps > 0.0 && media.frame_rate && *media.frame_rate > 0.0) {
        const double step = *media.frame_rate / request.target_fps;
        return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(step + 0.5));
    }
    return std::max<std::uint32_t>(1, request.every_nth);
}

// Pulls decoded frames from the appsink and hands every `step`-th one to
// `callback`, bounded by a wall-clock deadline and the abort signal. `detail`
// receives the bus error's message when the pipeline posts one.
DecodeEnd run_decode(GstElement *pipeline, GstElement *appsink, std::uint32_t step,
                     const VideoFrameCallback &callback, const std::function<bool()> &abort,
                     std::string &detail, std::uint64_t &delivered, std::uint32_t &width,
                     std::uint32_t &height)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    const guint64 start = g_get_monotonic_time();
    std::uint64_t source_frame = 0;
    std::vector<std::uint8_t> rgb;

    for (;;) {
        if (abort && abort()) {
            gst_object_unref(bus);
            return DecodeEnd::Cancelled;
        }

        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(appsink), 1 * GST_SECOND);
        if (sample == nullptr) {
            // No sample: either the stream ended, the pipeline errored, or the
            // per-pull timeout lapsed. Ask the bus (non-blocking) which.
            GstMessage *message = gst_bus_timed_pop_filtered(
                    bus, 0, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            if (message != nullptr) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                    gst_message_unref(message);
                    gst_object_unref(bus);
                    return DecodeEnd::Eos;
                }
                GError *error = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                if (error != nullptr) {
                    detail = error->message;
                }
                g_clear_error(&error);
                g_free(debug);
                gst_message_unref(message);
                gst_object_unref(bus);
                return DecodeEnd::Error;
            }
            if (g_get_monotonic_time() - start > kDecodeTimeoutUs) {
                gst_object_unref(bus);
                return DecodeEnd::Timeout;
            }
            continue;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstCaps *caps = gst_sample_get_caps(sample);
        GstVideoInfo info;
        if (buffer != nullptr && caps != nullptr && gst_video_info_from_caps(&info, caps)) {
            GstVideoFrame frame;
            if (gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
                const std::uint8_t *data =
                        static_cast<const std::uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                const gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
                const std::uint32_t w = static_cast<std::uint32_t>(info.width);
                const std::uint32_t h = static_cast<std::uint32_t>(info.height);
                const std::size_t row_bytes = static_cast<std::size_t>(w) * 3;
                if (stride >= static_cast<gint>(row_bytes)) {
                    rgb.resize(static_cast<std::size_t>(w) * h * 3);
                    for (std::uint32_t y = 0; y < h; ++y) {
                        std::memcpy(rgb.data() + static_cast<std::size_t>(y) * row_bytes,
                                    data + static_cast<std::size_t>(y) * stride, row_bytes);
                    }
                    gst_video_frame_unmap(&frame);
                    gst_sample_unref(sample);

                    if (source_frame % step == 0) {
                        if (delivered == 0) {
                            width = w;
                            height = h;
                        }
                        const FrameView view{ rgb.data(), w, h };
                        const bool keep = callback(view, static_cast<std::uint32_t>(source_frame));
                        ++delivered;
                        if (!keep) {
                            gst_object_unref(bus);
                            return DecodeEnd::Eos; // a clean stop
                        }
                    }
                    ++source_frame;
                    continue;
                }
                gst_video_frame_unmap(&frame);
            }
        }
        gst_sample_unref(sample);
        ++source_frame;
    }
}

} // namespace

VideoFramesResult extract_video_frames(const VideoFramesRequest &request,
                                       const VideoFrameCallback &callback,
                                       const std::function<bool()> &abort)
{
    VideoFramesResult result;

    // Probe first: it says whether there is a picture at all and its rate, so
    // the stream choice and the decimation step are settled before any decode.
    // It also separates a missing/unreadable source (open/decode failure) from
    // a source that opens but has no picture.
    const std::optional<NewMedia> media = probe(request.source);
    if (!media) {
        result.error = VideoFramesError::OpenOrDecode;
        result.message = "cannot open or probe source: " + request.source.string();
        return result;
    }
    if (!media->width || !media->height || *media->width == 0 || *media->height == 0) {
        result.error = VideoFramesError::NoVideoStream;
        result.message = "no video stream in source: " + request.source.string();
        return result;
    }

    const std::uint32_t step = decimation_step(request, *media);

    gchar *uri = gst_filename_to_uri(request.source.string().c_str(), nullptr);
    if (uri == nullptr) {
        result.error = VideoFramesError::PipelineError;
        result.message = "cannot turn the source path into a URI";
        return result;
    }

    GstElement *pipeline = gst_pipeline_new("video-frames");
    GstElement *decode = gst_element_factory_make("uridecodebin", "decode");
    GstElement *convert = gst_element_factory_make("videoconvert", "convert");
    GstElement *filter = gst_element_factory_make("capsfilter", "filter");
    GstElement *sink = gst_element_factory_make("appsink", "sink");
    if (pipeline == nullptr || decode == nullptr || convert == nullptr || filter == nullptr
        || sink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&decode);
        gst_clear_object(&convert);
        gst_clear_object(&filter);
        gst_clear_object(&sink);
        g_free(uri);
        result.error = VideoFramesError::PipelineError;
        result.message = "a pipeline element is missing from the GStreamer registry";
        return result;
    }

    g_object_set(decode, "uri", uri, nullptr);
    g_free(uri);

    // Pin the pixel format to packed RGB8 and let width/height follow the
    // source, so the delivered bytes are a plain width*height*3 array at the
    // source's own resolution (a mask is cut at the frame's true size).
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Throttle the decode to the pull rate: a one-buffer queue that does not
    // drop blocks the producer when full, so a slow consumer never piles up
    // unbounded memory. No display, so sync is off.
    g_object_set(sink, "sync", FALSE, "max-buffers", 1, "drop", FALSE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, convert, filter, sink, nullptr);
    if (!gst_element_link_many(convert, filter, sink, nullptr)) {
        gst_object_unref(pipeline);
        result.error = VideoFramesError::PipelineError;
        result.message = "cannot link the video pipeline";
        return result;
    }

    PadSelect select;
    select.pipeline = pipeline;
    select.convert = convert;
    select.target = request.stream_index;
    g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), &select);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        result.error = VideoFramesError::PipelineError;
        result.message = "the decode pipeline failed to start";
        return result;
    }

    std::string detail;
    const DecodeEnd end = run_decode(pipeline, sink, step, callback, abort, detail,
                                     result.frames_delivered, result.width, result.height);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    switch (end) {
    case DecodeEnd::Eos:
        // Clean end. A target video stream that never appeared still
        // reaches EOS with nothing delivered - that is an out-of-range
        // index, reported as NoVideoStream rather than an empty success.
        if (result.frames_delivered == 0 && request.stream_index > 0) {
            result.error = VideoFramesError::NoVideoStream;
            result.message =
                    "video stream " + std::to_string(request.stream_index) + " out of range";
        }
        return result;
    case DecodeEnd::Cancelled:
        result.error = VideoFramesError::Cancelled;
        result.message = "decode cancelled";
        return result;
    case DecodeEnd::Timeout:
        result.error = VideoFramesError::PipelineError;
        result.message = "decode pipeline timed out";
        return result;
    case DecodeEnd::Error:
        result.error = VideoFramesError::PipelineError;
        result.message = detail.empty() ? "decode pipeline failed" : detail;
        return result;
    }
    return result;
}

} // namespace genesis::adapters::engine::ges
