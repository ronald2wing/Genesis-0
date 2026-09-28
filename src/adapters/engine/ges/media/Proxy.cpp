// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/Proxy.h"

#include <cstdint>
#include <gst/gst.h>
#include <string>
#include <system_error>

#include "adapters/engine/ges/media/Probe.h"
#include "project/command/MediaCommands.h"
#include "render/ProxyCache.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::project::NewMedia;

// The width that keeps the source's aspect ratio at `target_height`, rounded
// down to an even number. VP8's 4:2:0 subsampling rejects odd dimensions, and
// a stand-in a pixel narrower than exact is still a faithful one.
std::uint32_t aspect_width(std::uint32_t source_width, std::uint32_t source_height,
                           std::uint32_t target_height)
{
    const std::uint64_t scaled = static_cast<std::uint64_t>(source_width) * target_height;
    std::uint32_t width =
            static_cast<std::uint32_t>((scaled + (source_height / 2)) / source_height);
    width &= ~std::uint32_t{ 1 };
    return width < 2 ? 2 : width;
}

// The requested height, held even and no smaller than two.
std::uint32_t even_height(std::uint32_t height)
{
    std::uint32_t even = height & ~std::uint32_t{ 1 };
    return even < 2 ? 2 : even;
}

// Wires the picture path when uridecodebin exposes it. Only the video stream
// feeds the proxy: a stand-in is a picture, and the audio pad, left unlinked,
// has its buffers dropped rather than blocking the decode.
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

// Transcodes `source` to `destination` at `width` x `height`. True when the
// pipeline reached EOS; the caller removes a partial destination on failure.
bool transcode(const std::filesystem::path &source, const std::filesystem::path &destination,
               std::uint32_t width, std::uint32_t height)
{
    // The decodebin only takes URIs; a bare path would read as a relative URI
    // and never resolve to the file.
    gchar *uri = gst_filename_to_uri(source.string().c_str(), nullptr);
    if (uri == nullptr) {
        return false;
    }

    GstElement *pipeline = gst_pipeline_new("proxy");
    GstElement *decode = gst_element_factory_make("uridecodebin", "decode");
    GstElement *convert = gst_element_factory_make("videoconvert", "convert");
    GstElement *scale = gst_element_factory_make("videoscale", "scale");
    GstElement *filter = gst_element_factory_make("capsfilter", "filter");
    GstElement *encoder = gst_element_factory_make("vp8enc", "encoder");
    GstElement *muxer = gst_element_factory_make("webmmux", "muxer");
    GstElement *sink = gst_element_factory_make("filesink", "sink");
    if (pipeline == nullptr || decode == nullptr || convert == nullptr || scale == nullptr
        || filter == nullptr || encoder == nullptr || muxer == nullptr || sink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&decode);
        gst_clear_object(&convert);
        gst_clear_object(&scale);
        gst_clear_object(&filter);
        gst_clear_object(&encoder);
        gst_clear_object(&muxer);
        gst_clear_object(&sink);
        g_free(uri);
        return false;
    }

    g_object_set(decode, "uri", uri, nullptr);
    g_free(uri);

    // The exact target size, so videoscale performs a real downscale instead
    // of keeping the source width and tagging a pixel-aspect-ratio correction
    // (videoscale's add-borders default). An anamorphic stand-in would read
    // back at the wrong size and players would stretch it.
    GstCaps *caps =
            gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, static_cast<gint>(width),
                                "height", G_TYPE_INT, static_cast<gint>(height), nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Real-time deadline: a proxy is a stand-in, not a master, so generation
    // trades bit-exactness for speed.
    g_object_set(encoder, "deadline", 1, nullptr);

    g_object_set(sink, "location", destination.string().c_str(), nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, convert, scale, filter, encoder, muxer, sink,
                     nullptr);
    if (!gst_element_link_many(convert, scale, filter, encoder, muxer, sink, nullptr)) {
        gst_object_unref(pipeline);
        return false;
    }
    g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), convert);

    // Drive to EOS and stop. A filesink proxy has no live consumer, so no
    // GMainLoop is needed: the bus's timed pop blocks until the pipeline ends
    // or posts an error.
    GstBus *bus = gst_element_get_bus(pipeline);
    const bool reached_eos = [&]() {
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            return false;
        }
        GstMessage *message = gst_bus_timed_pop_filtered(
                bus, GST_CLOCK_TIME_NONE,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message == nullptr) {
            return false;
        }
        const bool eos = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        gst_message_unref(message);
        return eos;
    }();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return reached_eos;
}

} // namespace

std::optional<std::filesystem::path> make_proxy(const ProxyRequest &request)
{
    // Probe first: the target width follows the source's aspect ratio, and a
    // file that cannot be probed (missing, unreadable, or pictureless) fails
    // before the destination is touched.
    const std::optional<NewMedia> media = probe(request.source);
    if (!media || !media->width || !media->height) {
        return std::nullopt;
    }
    const std::uint32_t source_width = *media->width;
    const std::uint32_t source_height = *media->height;
    if (source_width == 0 || source_height == 0) {
        return std::nullopt;
    }

    const std::uint32_t height = even_height(request.height);
    const std::uint32_t width = aspect_width(source_width, source_height, height);

    // Make the destination's directory before the pipeline runs, so filesink
    // never fails on a missing parent. A stale file is removed up front so a
    // failed run cannot leave a previous proxy behind.
    std::error_code ec;
    const std::filesystem::path parent = request.destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::nullopt;
        }
    }
    std::filesystem::remove(request.destination, ec);

    if (!transcode(request.source, request.destination, width, height)) {
        // A failed encode may still have written a partial file; a stand-in
        // that is incomplete must not be mistaken for a working one.
        std::filesystem::remove(request.destination, ec);
        return std::nullopt;
    }

    return request.destination;
}

std::optional<std::filesystem::path> ensure_proxy(const std::filesystem::path &source,
                                                  const std::filesystem::path &cache_root)
{
    if (source.empty() || cache_root.empty()) {
        return std::nullopt;
    }
    // The source's byte size at generation time is the key's cheap staleness
    // proxy; a source that cannot be statted (missing) has nothing to proxy.
    const std::uintmax_t source_size = genesis::render::cache_source_size(source);
    if (source_size == 0) {
        return std::nullopt;
    }

    const std::filesystem::path destination = genesis::render::proxy_cache_path(cache_root, source);
    const genesis::render::CacheKey key = genesis::render::proxy_key(source, source_size);

    // A fresh proxy is reused as-is; a missing or stale one is regenerated and
    // then recorded, so the next build finds it fresh. A generation that fails
    // leaves no record behind - the caller reads the source at full size.
    if (genesis::render::cache_is_fresh(destination, key)) {
        return destination;
    }
    const std::optional<std::filesystem::path> made =
            make_proxy(ProxyRequest{ source, destination });
    if (!made) {
        return std::nullopt;
    }
    if (!genesis::render::cache_record(destination, key)) {
        return std::nullopt;
    }
    return destination;
}

} // namespace genesis::adapters::engine::ges
