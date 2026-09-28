// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/Filmstrip.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/video/video-frame.h>
#include <gst/video/video-info.h>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "adapters/engine/ges/media/Probe.h"
#include "project/command/MediaCommands.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::project::NewMedia;

// The poster's width, fixed because the request carries no size; the height
// follows the source's aspect ratio.
constexpr std::uint32_t kPosterWidth = 480;

// The pixel encoder: JPEG, via gst-plugins-good's `jpegenc`. Both the poster
// and the filmstrip are single stills, and JPEG is the compact format the
// launch screen and the timeline thumbnails want; the filmstrip is one image
// so the timeline draws it as one texture upload.
// The number of tiles across, held to a 1..=60 range.
constexpr std::uint32_t kMaxColumns = 60;

// The even height that keeps `width`'s aspect at the source's ratio, matching
// the scaler's preference for even dimensions. Reused for the tile and the
// poster.
std::uint32_t aspect_height(std::uint32_t width, std::uint32_t source_width,
                            std::uint32_t source_height)
{
    std::uint32_t height = static_cast<std::uint32_t>(
            ((static_cast<std::uint64_t>(width) * source_height) + (source_width / 2))
            / source_width);
    height &= ~std::uint32_t{ 1 };
    return height < 2 ? 2 : height;
}

// Wires the picture path when uridecodebin exposes it. Only the video stream
// feeds the grab: an audio pad is left unlinked, and its buffers are dropped
// rather than blocking the decode - the same shape Proxy.cpp uses.
void on_pad_added(GstElement *source, GstPad *pad, gpointer user_data)
{
    (void)source;
    auto *convert = static_cast<GstElement *>(user_data);

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    if (caps == nullptr) {
        return;
    }
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    const gchar *name = structure == nullptr ? nullptr : gst_structure_get_name(structure);
    const bool is_video = name != nullptr && g_str_has_prefix(name, "video/");
    gst_caps_unref(caps);
    if (!is_video) {
        return;
    }

    GstPad *sink = gst_element_get_static_pad(convert, "sink");
    if (sink != nullptr) {
        if (!gst_pad_is_linked(sink)) {
            gst_pad_link(pad, sink);
        }
        gst_object_unref(sink);
    }
}

// Decodes one frame of `source` at `seconds`, scaled to `width` x `height`,
// as packed RGB (3 bytes per pixel, row-major). Returns nullopt when the file
// cannot be decoded or no frame lands within the timeout.
//
// Mechanism: a seek loop. One decode pipeline prerolls to PAUSED, flush-seeks
// to the target instant, and pulls the one frame the appsink then holds. For a
// thumbnail the nearest decodable frame at-or-after the target is exact enough
// - the same "seek to the keyframe and walk forward" behavior the filmstrip
// uses. Building one pipeline per tile is deliberately
// stateless: a fresh pipeline cannot carry a stale position from the tile
// before.
std::optional<std::vector<std::uint8_t>> grab_rgb(const std::filesystem::path &source,
                                                  double seconds, std::uint32_t width,
                                                  std::uint32_t height)
{
    gchar *uri = gst_filename_to_uri(source.string().c_str(), nullptr);
    if (uri == nullptr) {
        return std::nullopt;
    }

    GstElement *pipeline = gst_pipeline_new("grab");
    GstElement *decode = gst_element_factory_make("uridecodebin", "decode");
    GstElement *convert = gst_element_factory_make("videoconvert", "convert");
    GstElement *scale = gst_element_factory_make("videoscale", "scale");
    GstElement *filter = gst_element_factory_make("capsfilter", "filter");
    GstElement *sink = gst_element_factory_make("appsink", "sink");
    if (pipeline == nullptr || decode == nullptr || convert == nullptr || scale == nullptr
        || filter == nullptr || sink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&decode);
        gst_clear_object(&convert);
        gst_clear_object(&scale);
        gst_clear_object(&filter);
        gst_clear_object(&sink);
        g_free(uri);
        return std::nullopt;
    }

    g_object_set(decode, "uri", uri, nullptr);
    g_free(uri);

    // Force one packed RGB size, so videoscale performs a real scale and the
    // grabbed bytes are a plain width*height*3 array with a known stride.
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width",
                                        G_TYPE_INT, static_cast<gint>(width), "height", G_TYPE_INT,
                                        static_cast<gint>(height), nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Drain rather than clock (no display): hold one frame, drop the rest.
    g_object_set(sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, convert, scale, filter, sink, nullptr);
    if (!gst_element_link_many(convert, scale, filter, sink, nullptr)) {
        gst_object_unref(pipeline);
        return std::nullopt;
    }
    g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), convert);

    // Stop: tear the pipeline down and report a miss.
    const auto stop = [&]() {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return std::optional<std::vector<std::uint8_t>>{ std::nullopt };
    };

    // Preroll the first frame at PAUSED, as the session's preroll does.
    if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        return stop();
    }
    GstState current = GST_STATE_VOID_PENDING;
    if (gst_element_get_state(pipeline, &current, nullptr, 5 * GST_SECOND)
                == GST_STATE_CHANGE_FAILURE
        || current != GST_STATE_PAUSED) {
        return stop();
    }

    // Seek to the target instant. A seek that cannot land (past the end, an
    // index that refuses) is not fatal: the try_pull below is what decides
    // whether a frame exists to show.
    if (seconds > 0.0) {
        const GstClockTime target =
                static_cast<GstClockTime>(std::llround(seconds * static_cast<double>(GST_SECOND)));
        gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
                         static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                         GST_SEEK_TYPE_SET, target, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
        gst_element_get_state(pipeline, &current, nullptr, 5 * GST_SECOND);
    }

    // A preroll sample, not a regular one: the pipeline is PAUSED, so the
    // frame that prerolls into the sink is the one try_pull_preroll reads.
    // try_pull_sample would wait on a regular sample that never comes while
    // paused, and return null.
    GstSample *sample = gst_app_sink_try_pull_preroll(GST_APP_SINK(sink), 5 * GST_SECOND);
    if (sample == nullptr) {
        return stop();
    }

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *sample_caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    if (buffer == nullptr || sample_caps == nullptr
        || !gst_video_info_from_caps(&info, sample_caps)) {
        gst_sample_unref(sample);
        return stop();
    }

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return stop();
    }

    const std::uint8_t *data =
            static_cast<const std::uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 3;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(width) * height * 3);
    if (stride < static_cast<gint>(row_bytes)) {
        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);
        return stop();
    }
    for (std::uint32_t y = 0; y < height; ++y) {
        std::memcpy(rgb.data() + (static_cast<std::size_t>(y) * row_bytes),
                    data + (static_cast<std::size_t>(y) * stride), row_bytes);
    }
    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return rgb;
}

// Encodes `width` x `height` packed RGB to a JPEG at `destination`. The
// reverse half of grab_rgb: one raw frame pushed through an appsrc into
// jpegenc and filesink, driven to EOS. True when EOS was reached.
bool write_jpeg(const std::filesystem::path &destination, std::span<const std::uint8_t> rgb,
                std::uint32_t width, std::uint32_t height)
{
    GstElement *pipeline = gst_pipeline_new("jpeg");
    GstElement *src = gst_element_factory_make("appsrc", "src");
    GstElement *convert = gst_element_factory_make("videoconvert", "convert");
    GstElement *encoder = gst_element_factory_make("jpegenc", "encoder");
    GstElement *sink = gst_element_factory_make("filesink", "sink");
    if (pipeline == nullptr || src == nullptr || convert == nullptr || encoder == nullptr
        || sink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&src);
        gst_clear_object(&convert);
        gst_clear_object(&encoder);
        gst_clear_object(&sink);
        return false;
    }

    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width",
                                        G_TYPE_INT, static_cast<gint>(width), "height", G_TYPE_INT,
                                        static_cast<gint>(height), "framerate", GST_TYPE_FRACTION,
                                        1, 1, nullptr);
    g_object_set(src, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(sink, "location", destination.string().c_str(), nullptr);

    gst_bin_add_many(GST_BIN(pipeline), src, convert, encoder, sink, nullptr);
    if (!gst_element_link_many(src, convert, encoder, sink, nullptr)) {
        gst_object_unref(pipeline);
        return false;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_object_unref(pipeline);
        return false;
    }

    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, rgb.size(), nullptr);
    const gssize filled = gst_buffer_fill(buffer, 0, rgb.data(), rgb.size());
    const GstFlowReturn pushed = filled == static_cast<gssize>(rgb.size())
            ? gst_app_src_push_buffer(GST_APP_SRC(src), buffer)
            : [&]() {
                  gst_buffer_unref(buffer);
                  return GST_FLOW_ERROR;
              }();
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool eos = pushed == GST_FLOW_OK && message != nullptr
            && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message != nullptr) {
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return eos;
}

// Removes any stale destination and makes its directory, so filesink never
// fails on a missing parent and a failed run cannot leave an old image behind.
bool prepare_destination(const std::filesystem::path &destination)
{
    std::error_code ec;
    const std::filesystem::path parent = destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }
    std::filesystem::remove(destination, ec);
    return true;
}

} // namespace

std::optional<std::filesystem::path> make_filmstrip(const FilmstripRequest &request)
{
    // Probe first: the tile height follows the source's aspect and the window
    // follows its duration, and a file that cannot be probed (missing,
    // unreadable, or pictureless) fails before the destination is touched.
    const std::optional<NewMedia> media = probe(request.source);
    if (!media || !media->width || !media->height) {
        return std::nullopt;
    }
    const std::uint32_t source_width = *media->width;
    const std::uint32_t source_height = *media->height;
    if (source_width == 0 || source_height == 0) {
        return std::nullopt;
    }

    const std::uint32_t columns = std::clamp(request.columns, 1U, kMaxColumns);
    const std::uint32_t tile_width = request.tile_width < 2 ? 2 : request.tile_width;
    const std::uint32_t tile_height = aspect_height(tile_width, source_width, source_height);

    // The window, inside the file. With no duration there is nothing to space
    // tiles across, so the request refuses rather than guessing.
    const double duration = media->duration ? media->duration->as_double() : 0.0;
    if (!(duration > 0.0)) {
        return std::nullopt;
    }
    const double from = std::clamp(request.start, 0.0, duration);
    const double to =
            request.duration > 0.0 ? std::min(from + request.duration, duration) : duration;
    const double span = to - from;
    if (!(span > 0.0)) {
        return std::nullopt;
    }

    if (!prepare_destination(request.destination)) {
        return std::nullopt;
    }

    // Sample the middle of each slice and paint the tiles onto a black canvas,
    // left to right. A tile whose instant will not decode stays black - the
    // same canvas, and the same "never a hole" base a timeline can still
    // draw.
    const std::size_t tile_bytes = static_cast<std::size_t>(tile_width) * tile_height * 3;
    std::vector<std::uint8_t> grid(static_cast<std::size_t>(columns) * tile_bytes, 0);
    for (std::uint32_t tile = 0; tile < columns; ++tile) {
        const double at =
                from + (span * (static_cast<double>(tile) + 0.5) / static_cast<double>(columns));
        const std::optional<std::vector<std::uint8_t>> frame =
                grab_rgb(request.source, at, tile_width, tile_height);
        if (!frame || frame->size() != tile_bytes) {
            continue;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(tile_width) * 3;
        for (std::uint32_t y = 0; y < tile_height; ++y) {
            const std::size_t dst = ((static_cast<std::size_t>(y) * columns * tile_width)
                                     + (static_cast<std::size_t>(tile) * tile_width))
                    * 3;
            std::memcpy(grid.data() + dst,
                        frame->data() + (static_cast<std::size_t>(y) * row_bytes), row_bytes);
        }
    }

    if (!write_jpeg(request.destination, grid, columns * tile_width, tile_height)) {
        std::error_code ec;
        std::filesystem::remove(request.destination, ec);
        return std::nullopt;
    }
    return request.destination;
}

std::optional<std::filesystem::path> make_poster(const std::filesystem::path &source,
                                                 const std::filesystem::path &destination,
                                                 double at)
{
    const std::optional<NewMedia> media = probe(source);
    if (!media || !media->width || !media->height) {
        return std::nullopt;
    }
    const std::uint32_t source_width = *media->width;
    const std::uint32_t source_height = *media->height;
    if (source_width == 0 || source_height == 0) {
        return std::nullopt;
    }
    const std::uint32_t height = aspect_height(kPosterWidth, source_width, source_height);

    const std::optional<std::vector<std::uint8_t>> frame =
            grab_rgb(source, std::max(at, 0.0), kPosterWidth, height);
    if (!frame || frame->size() != static_cast<std::size_t>(kPosterWidth) * height * 3) {
        return std::nullopt;
    }

    if (!prepare_destination(destination)) {
        return std::nullopt;
    }
    if (!write_jpeg(destination, *frame, kPosterWidth, height)) {
        std::error_code ec;
        std::filesystem::remove(destination, ec);
        return std::nullopt;
    }
    return destination;
}

} // namespace genesis::adapters::engine::ges
