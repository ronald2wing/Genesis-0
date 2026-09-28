// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/Reverse.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include "adapters/engine/ges/media/Probe.h"
#include "project/command/MediaCommands.h"
#include "render/ExportClip.h"
#include "render/ReverseCache.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::core::Rational;
using genesis::project::NewMedia;

// The reverse holds the span's decoded frames in memory while it turns them
// round. This bounds that memory: a span whose decoded frames would need more
// than this is refused rather than attempted (see make_reverse). GStreamer has
// no reverse-decode element on this machine, so a two-pass hold-and-reorder is
// the mechanism; the honest cost of it is that the span must fit in memory.
constexpr std::size_t kMaxBufferedBytes = 512U << 20;

// The source's frame rate as an exact fraction, or nullopt when the probe
// found no rate to time the reversed output by. The exact fraction is
// preferred; the decimal is the fallback through the documented microsecond
// seam (Rational::approximate).
std::optional<Rational> source_frame_rate(const NewMedia &media)
{
    if (media.frame_rate_fraction) {
        const std::optional<Rational> exact = Rational::parse(*media.frame_rate_fraction);
        if (exact && !exact->is_zero() && !exact->is_negative()) {
            return exact;
        }
    }
    if (media.frame_rate && *media.frame_rate > 0.0) {
        const std::optional<Rational> approx = Rational::approximate(*media.frame_rate);
        if (approx && !approx->is_zero()) {
            return approx;
        }
    }
    return std::nullopt;
}

// Wires the picture path when uridecodebin exposes it, and drops audio the way
// Proxy.cpp does: only the video stream feeds the reverse, and an audio pad
// left unlinked would block the decode, so its buffers are dropped instead.
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

// Whether an error is already posted on the pipeline's bus. Used after a null
// sample pull to tell a clean EOS (frames ran out) from a decode failure.
bool bus_has_error(GstElement *pipeline)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    const bool has_error = message != nullptr;
    if (message != nullptr) {
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    return has_error;
}

// Pass one of the reverse: decodes the source's span [from, to) forwards into
// raw I420 frames, one sample per frame in play order, appended to `out`
// (which the caller owns and releases). Returns false on any failure, including
// a span that decodes to no frames. The first frame kept is the first decodable
// one at-or-after `from` (a seek lands on the preceding keyframe and the
// decoder walks forward), and the first frame whose PTS reaches `to` is
// dropped: the span is half-open.
bool decode_forward(const std::filesystem::path &source, GstClockTime from_ns, GstClockTime to_ns,
                    std::uint32_t width, std::uint32_t height, std::size_t max_frames,
                    std::vector<GstSample *> &out)
{
    gchar *uri = gst_filename_to_uri(source.string().c_str(), nullptr);
    if (uri == nullptr) {
        return false;
    }

    GstElement *pipeline = gst_pipeline_new("reverse-decode");
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
        return false;
    }

    g_object_set(decode, "uri", uri, nullptr);
    g_free(uri);

    // Force one exact I420 size, so every buffered frame is a fixed
    // width*height*3/2 array the encode pass can push back out unchanged.
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", "width",
                                        G_TYPE_INT, static_cast<gint>(width), "height", G_TYPE_INT,
                                        static_cast<gint>(height), nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Drain rather than clock (headless): no drop, so every frame is pulled.
    g_object_set(sink, "sync", FALSE, "drop", FALSE, "max-buffers", 0, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, convert, scale, filter, sink, nullptr);
    if (!gst_element_link_many(convert, scale, filter, sink, nullptr)) {
        gst_object_unref(pipeline);
        return false;
    }
    g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), convert);

    const auto stop = [&]() {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    };

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        return stop();
    }
    GstState state = GST_STATE_VOID_PENDING;
    if (gst_element_get_state(pipeline, &state, nullptr, 5 * GST_SECOND) == GST_STATE_CHANGE_FAILURE
        || state != GST_STATE_PLAYING) {
        return stop();
    }
    if (from_ns > 0) {
        gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
                         static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                         GST_SEEK_TYPE_SET, from_ns, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    }

    while (true) {
        if (out.size() >= max_frames) {
            // The span outgrew the memory bound: refuse rather than truncate.
            return stop();
        }
        GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
        if (sample == nullptr) {
            if (bus_has_error(pipeline)) {
                return stop();
            }
            // Clean EOS (the span reached the file's end) ends the pass; a
            // timeout with neither EOS nor error is a stall.
            if (gst_app_sink_is_eos(GST_APP_SINK(sink))) {
                break;
            }
            return stop();
        }
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        const GstClockTime pts = buffer != nullptr ? GST_BUFFER_PTS(buffer) : GST_CLOCK_TIME_NONE;
        if (pts != GST_CLOCK_TIME_NONE && pts >= to_ns) {
            gst_sample_unref(sample);
            break;
        }
        out.push_back(sample);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return !out.empty();
}

// Pass two of the reverse: encodes the I420 frames (in play order) backwards
// into `destination` as VP8/WebM, re-timing each so the reversed picture plays
// at the source's rate and starts at zero. True when the pipeline reached EOS;
// the caller removes a partial destination on failure.
bool encode_reversed(const std::filesystem::path &destination,
                     const std::vector<GstSample *> &frames, std::uint32_t width,
                     std::uint32_t height, gint fps_num, gint fps_den, GstClockTime frame_ns)
{
    GstElement *pipeline = gst_pipeline_new("reverse-encode");
    GstElement *src = gst_element_factory_make("appsrc", "src");
    GstElement *convert = gst_element_factory_make("videoconvert", "convert");
    GstElement *encoder = gst_element_factory_make("vp8enc", "encoder");
    GstElement *muxer = gst_element_factory_make("webmmux", "muxer");
    GstElement *sink = gst_element_factory_make("filesink", "sink");
    if (pipeline == nullptr || src == nullptr || convert == nullptr || encoder == nullptr
        || muxer == nullptr || sink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&src);
        gst_clear_object(&convert);
        gst_clear_object(&encoder);
        gst_clear_object(&muxer);
        gst_clear_object(&sink);
        return false;
    }

    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", "width",
                                        G_TYPE_INT, static_cast<gint>(width), "height", G_TYPE_INT,
                                        static_cast<gint>(height), "framerate", GST_TYPE_FRACTION,
                                        fps_num, fps_den, nullptr);
    g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, "do-timestamp", FALSE, nullptr);
    gst_caps_unref(caps);

    // Real-time deadline: a reverse is a cached copy, not a master, so
    // generation trades bit-exactness for speed, like the proxy does.
    g_object_set(encoder, "deadline", 1, nullptr);
    g_object_set(sink, "location", destination.string().c_str(), nullptr);

    gst_bin_add_many(GST_BIN(pipeline), src, convert, encoder, muxer, sink, nullptr);
    if (!gst_element_link_many(src, convert, encoder, muxer, sink, nullptr)) {
        gst_object_unref(pipeline);
        return false;
    }

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        gst_object_unref(pipeline);
        return false;
    }

    // Push last to first, re-timing each so the reversed picture keeps the
    // source's rate. Each buffer is an independent reference: the sample it
    // came from still owns its own, so releasing the samples later cannot
    // free a buffer appsrc is still holding.
    bool ok = true;
    GstClockTime output_time = 0;
    for (auto it = frames.rbegin(); it != frames.rend() && ok; ++it) {
        GstBuffer *buffer = gst_sample_get_buffer(*it);
        if (buffer == nullptr) {
            ok = false;
            break;
        }
        GstBuffer *out_buffer = gst_buffer_ref(buffer);
        GST_BUFFER_PTS(out_buffer) = output_time;
        GST_BUFFER_DURATION(out_buffer) = frame_ns;
        output_time += frame_ns;
        ok = gst_app_src_push_buffer(GST_APP_SRC(src), out_buffer) == GST_FLOW_OK;
    }
    gst_app_src_end_of_stream(GST_APP_SRC(src));

    // Only wait for the mux to finish when every frame was pushed: a push that
    // already failed cannot reach EOS, so waiting would just burn the timeout.
    if (ok) {
        GstBus *bus = gst_element_get_bus(pipeline);
        GstMessage *message = gst_bus_timed_pop_filtered(
                bus, 60 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        ok = message != nullptr && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        if (message != nullptr) {
            gst_message_unref(message);
        }
        gst_object_unref(bus);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}

// The frames a reverse holds, released whatever the outcome: every sample
// taken in pass one must be unreferenced whether pass two succeeds or not.
struct HeldFrames
{
    std::vector<GstSample *> samples;
    ~HeldFrames()
    {
        for (GstSample *sample : samples) {
            gst_sample_unref(sample);
        }
    }
};

} // namespace

std::optional<std::filesystem::path> make_reverse(const ReverseRequest &request)
{
    // Probe first: the span follows the source's duration and rate, and a file
    // that cannot be probed (missing, unreadable, or pictureless) fails before
    // the destination is touched.
    const std::optional<NewMedia> media = probe(request.source);
    if (!media || !media->width || !media->height) {
        return std::nullopt;
    }
    const std::uint32_t source_width = *media->width;
    const std::uint32_t source_height = *media->height;
    if (source_width == 0 || source_height == 0) {
        return std::nullopt;
    }

    // The picture must reverse to an even size: VP8's 4:2:0 subsampling
    // rejects odd dimensions, and a picture a pixel narrower than exact is a
    // faithful reverse - the same stand-in rule Proxy.cpp applies.
    std::uint32_t width = source_width & ~std::uint32_t{ 1 };
    if (width < 2) {
        width = 2;
    }
    std::uint32_t height = source_height & ~std::uint32_t{ 1 };
    if (height < 2) {
        height = 2;
    }

    if (!std::isfinite(request.start) || !std::isfinite(request.duration)
        || request.duration < 0.0) {
        return std::nullopt;
    }
    const double file_duration = media->duration ? media->duration->as_double() : 0.0;

    // The window, inside the file. A span with no known end and no explicit
    // duration is nothing to turn round.
    double from = request.start < 0.0 ? 0.0 : request.start;
    double to = 0.0;
    if (request.duration > 0.0) {
        to = from + request.duration;
        if (file_duration > 0.0) {
            from = std::min(from, file_duration);
            to = std::min(to, file_duration);
        }
    } else {
        if (!(file_duration > 0.0)) {
            return std::nullopt;
        }
        from = std::min(from, file_duration);
        to = file_duration;
    }
    if (!(to > from)) {
        return std::nullopt;
    }

    const std::optional<Rational> fps = source_frame_rate(*media);
    if (!fps) {
        return std::nullopt;
    }
    const GstClockTime frame_ns = static_cast<GstClockTime>(
            std::llround(fps->recip().as_double() * static_cast<double>(GST_SECOND)));
    const gint fps_num = static_cast<gint>(fps->numerator());
    const gint fps_den = static_cast<gint>(fps->denominator());
    const GstClockTime from_ns =
            static_cast<GstClockTime>(std::llround(from * static_cast<double>(GST_SECOND)));
    const GstClockTime to_ns =
            static_cast<GstClockTime>(std::llround(to * static_cast<double>(GST_SECOND)));

    // The memory bound, in frames: the span's decoded I420 bytes must fit.
    // The decode pass stops at `to` or the file's end anyway; this is the
    // guard that refuses a span too long to hold whole in memory.
    const std::size_t frame_bytes = static_cast<std::size_t>(width) * height * 3 / 2;
    const std::size_t max_frames = std::max<std::size_t>(kMaxBufferedBytes / frame_bytes, 2);

    // Refuse a span whose frame count would exceed the bound before decoding,
    // so a too-long reverse fails fast instead of decoding and then truncating.
    const double span_frames = (to - from) * fps->as_double();
    if (span_frames > static_cast<double>(max_frames)) {
        return std::nullopt;
    }

    // Make the destination's directory, and remove any stale file, before the
    // pipeline runs so filesink never fails on a missing parent and a failed
    // run cannot leave a previous reverse behind.
    std::error_code ec;
    const std::filesystem::path parent = request.destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return std::nullopt;
        }
    }
    std::filesystem::remove(request.destination, ec);

    HeldFrames held;
    if (!decode_forward(request.source, from_ns, to_ns, width, height, max_frames, held.samples)) {
        std::filesystem::remove(request.destination, ec);
        return std::nullopt;
    }
    if (!encode_reversed(request.destination, held.samples, width, height, fps_num, fps_den,
                         frame_ns)) {
        std::filesystem::remove(request.destination, ec);
        return std::nullopt;
    }

    return request.destination;
}

std::optional<std::filesystem::path> ensure_reverse(const std::filesystem::path &source,
                                                    double start, double duration,
                                                    const std::filesystem::path &cache_root)
{
    if (source.empty() || cache_root.empty()) {
        return std::nullopt;
    }
    // The source's byte size at generation time is the key's cheap staleness
    // proxy; a source that cannot be statted (missing) has nothing to reverse.
    const std::uintmax_t source_size = genesis::render::cache_source_size(source);
    if (source_size == 0) {
        return std::nullopt;
    }

    const std::filesystem::path destination =
            genesis::render::reverse_cache_path(cache_root, source, start, duration);
    const genesis::render::CacheKey key =
            genesis::render::reverse_key(source, source_size, start, duration);

    // A fresh copy is reused as-is; a missing or stale one is regenerated and
    // then recorded, so the next build finds it fresh. A generation that fails
    // leaves no record behind - the caller reads the source forward.
    if (genesis::render::cache_is_fresh(destination, key)) {
        return destination;
    }
    const std::optional<std::filesystem::path> made =
            make_reverse(ReverseRequest{ source, destination, start, duration });
    if (!made) {
        return std::nullopt;
    }
    if (!genesis::render::cache_record(destination, key)) {
        return std::nullopt;
    }
    return destination;
}

void ensure_reverses(std::span<const genesis::render::ExportClip> clips,
                     const std::filesystem::path &cache_root,
                     const std::function<bool()> &cancelled)
{
    for (const genesis::render::ExportClip &clip : clips) {
        if (cancelled && cancelled()) {
            return;
        }
        // Only footage plays backwards; a still, a title or a sound-only clip
        // has no reversible span, and a clip with no source has nothing to
        // reverse.
        if (!clip.reverse || clip.kind != genesis::render::ClipKind::Video || clip.path.empty()) {
            continue;
        }
        ensure_reverse(clip.path, clip.source_start.as_double(), clip.duration.as_double(),
                       cache_root);
    }
}

} // namespace genesis::adapters::engine::ges
