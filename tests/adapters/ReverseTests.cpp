// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video-frame.h>
#include <gst/video/video-info.h>

#include "adapters/engine/ges/media/Probe.h"
#include "adapters/engine/ges/media/Reverse.h"
#include "project/command/MediaCommands.h"

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
using genesis::project::NewMedia;

// Non-throwing existence+size probe, so a check that the file was made can
// fail (and report) instead of aborting the suite when it was not.
bool non_empty(const std::filesystem::path &path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec && std::filesystem::file_size(path, ec) > 0
            && !ec;
}

// A one-second 320x240 VP8/WebM clip of two colours: 15 red frames then 15
// green frames. The temporal asymmetry is what makes a reverse observable:
// the copy must start green (the input's end) and end red (the input's start).
// `concat` stitches the two runs into one stream before the single mux.
std::string source_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-reverse-source.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q "
                                "videotestsrc pattern=red num-buffers=15 ! "
                                "video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! "
                                "concat name=c "
                                "videotestsrc pattern=green num-buffers=15 ! "
                                "video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! c. "
                                "c. ! vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    const bool ok = std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
            && std::filesystem::file_size(generated) > 0;
    return ok ? generated.string() : std::string{ };
}

// Wires the picture path when uridecodebin exposes it, like the adapter does.
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

// Decodes one frame of `file` at `seconds` and returns its average RGB, or
// nullopt when the frame cannot be grabbed. Downscaled to 16x16 so the
// average is cheap; the fixture's solid colours stay solid through the scale.
std::optional<std::array<double, 3>> average_colour(const std::filesystem::path &file,
                                                    double seconds)
{
    gchar *uri = gst_filename_to_uri(file.string().c_str(), nullptr);
    if (uri == nullptr) {
        return std::nullopt;
    }

    GstElement *pipeline = gst_pipeline_new("colour");
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

    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width",
                                        G_TYPE_INT, 16, "height", G_TYPE_INT, 16, nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    // Hold one frame, drop the rest; headless, so no clock.
    g_object_set(sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, convert, scale, filter, sink, nullptr);
    if (!gst_element_link_many(convert, scale, filter, sink, nullptr)) {
        gst_object_unref(pipeline);
        return std::nullopt;
    }
    g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), convert);

    const auto stop = [&]() {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return std::optional<std::array<double, 3>>{ std::nullopt };
    };

    // Preroll, then seek, then pull the one prerolled frame, the way the
    // adapter's poster grab does.
    if (gst_element_set_state(pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
        return stop();
    }
    GstState state = GST_STATE_VOID_PENDING;
    if (gst_element_get_state(pipeline, &state, nullptr, 5 * GST_SECOND) == GST_STATE_CHANGE_FAILURE
        || state != GST_STATE_PAUSED) {
        return stop();
    }
    if (seconds > 0.0) {
        const GstClockTime target =
                static_cast<GstClockTime>(std::llround(seconds * static_cast<double>(GST_SECOND)));
        gst_element_seek(pipeline, 1.0, GST_FORMAT_TIME,
                         static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                         GST_SEEK_TYPE_SET, target, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
        gst_element_get_state(pipeline, &state, nullptr, 5 * GST_SECOND);
    }

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

    double sum[3] = { 0.0, 0.0, 0.0 };
    const std::uint8_t *data =
            static_cast<const std::uint8_t *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const gint frame_width = GST_VIDEO_INFO_WIDTH(&info);
    const gint frame_height = GST_VIDEO_INFO_HEIGHT(&info);
    for (gint y = 0; y < frame_height; ++y) {
        for (gint x = 0; x < frame_width; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(stride)
                    + static_cast<std::size_t>(x) * 3;
            sum[0] += data[at + 0];
            sum[1] += data[at + 1];
            sum[2] += data[at + 2];
        }
    }
    const double count = static_cast<double>(frame_width) * frame_height;

    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return std::array<double, 3>{ sum[0] / count, sum[1] / count, sum[2] / count };
}

void test_reverses_a_span()
{
    const std::string source = source_fixture();
    check(!source.empty(), "a source fixture is available");
    if (source.empty()) {
        return;
    }

    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-reverse-output.webm";
    std::filesystem::remove(destination);

    ges_adapter::ReverseRequest request;
    request.source = source;
    request.destination = destination;
    request.start = 0.0;
    request.duration = 0.0; // the whole file

    const std::optional<std::filesystem::path> made = ges_adapter::make_reverse(request);
    check(made.has_value(), "make_reverse reports success");
    check(made.has_value() && *made == destination, "make_reverse returns the destination path");
    check(std::filesystem::exists(destination), "the reverse file exists");
    check(non_empty(destination), "the reverse file is non-empty");

    const std::optional<NewMedia> probed = ges_adapter::probe(destination);
    check(probed.has_value(), "the reverse probes");
    if (probed.has_value() && probed->duration.has_value()) {
        const double duration = probed->duration->as_double();
        check(std::fabs(duration - 1.0) < 0.1,
              "the reverse duration matches the whole span (about one second)");
    }

    // The content is actually reversed: the input starts red and ends green,
    // so the copy must start green. Each frame is decoded and averaged; the
    // dominance checks tolerate VP8's re-encode of a solid colour.
    const std::optional<std::array<double, 3>> in_first = average_colour(source, 0.0);
    const std::optional<std::array<double, 3>> in_last = average_colour(source, 0.95);
    const std::optional<std::array<double, 3>> out_first = average_colour(destination, 0.0);
    check(in_first.has_value() && in_last.has_value() && out_first.has_value(),
          "frames decode for the colour check");
    if (in_first.has_value() && in_last.has_value() && out_first.has_value()) {
        check((*in_first)[0] > (*in_first)[1] * 2.0 && (*in_first)[0] > (*in_first)[2] * 2.0,
              "the input's first frame is red");
        check((*in_last)[1] > (*in_last)[0] * 2.0 && (*in_last)[1] > (*in_last)[2] * 2.0,
              "the input's last frame is green");
        check((*out_first)[1] > (*out_first)[0] * 2.0 && (*out_first)[1] > (*out_first)[2] * 2.0,
              "the copy's first frame is green (the input's end, not its start)");
    }

    std::filesystem::remove(destination);
    std::filesystem::remove(source);
}

void test_reverses_a_sub_span()
{
    const std::string source = source_fixture();
    check(!source.empty(), "a source fixture is available");
    if (source.empty()) {
        return;
    }

    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-reverse-subspan.webm";
    std::filesystem::remove(destination);

    // The second half only: a half-second span, which a full-file reverse
    // would have doubled.
    ges_adapter::ReverseRequest request;
    request.source = source;
    request.destination = destination;
    request.start = 0.5;
    request.duration = 0.5;

    const std::optional<std::filesystem::path> made = ges_adapter::make_reverse(request);
    check(made.has_value(), "a sub-span reverse reports success");
    check(non_empty(destination), "the sub-span reverse file is non-empty");

    const std::optional<NewMedia> probed = ges_adapter::probe(destination);
    if (probed.has_value() && probed->duration.has_value()) {
        const double duration = probed->duration->as_double();
        check(std::fabs(duration - 0.5) < 0.1,
              "the sub-span reverse is about half a second, not the whole file");
    }

    std::filesystem::remove(destination);
    std::filesystem::remove(source);
}

void test_a_missing_source_fails_cleanly()
{
    const std::filesystem::path missing =
            std::filesystem::temp_directory_path() / "genesis-reverse-missing-source.webm";
    std::filesystem::remove(missing);
    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-reverse-never-created.webm";
    std::filesystem::remove(destination);

    ges_adapter::ReverseRequest request;
    request.source = missing;
    request.destination = destination;

    check(!ges_adapter::make_reverse(request).has_value(), "a missing source returns nullopt");
    check(!std::filesystem::exists(destination), "a missing source creates no destination");
}

void test_a_failed_reverse_leaves_no_partial_file()
{
    const std::string source = source_fixture();
    check(!source.empty(), "a source fixture is available");
    if (source.empty()) {
        return;
    }

    // A header-only WebM: the discoverer still reports a video of one second,
    // but the body that held the frames is gone, so the decode pass finds
    // nothing and the reverse must fail - after the destination was prepared.
    const std::filesystem::path empty =
            std::filesystem::temp_directory_path() / "genesis-reverse-empty.webm";
    {
        std::error_code ec;
        std::filesystem::copy_file(source, empty, ec);
        if (ec) {
            std::filesystem::remove(source);
            check(false, "a header-only fixture can be written");
            return;
        }
        std::filesystem::resize_file(empty, 600, ec);
    }

    const std::filesystem::path destination =
            std::filesystem::temp_directory_path() / "genesis-reverse-partial.webm";
    std::filesystem::remove(destination);

    ges_adapter::ReverseRequest request;
    request.source = empty;
    request.destination = destination;

    check(!ges_adapter::make_reverse(request).has_value(),
          "a source with no decodable frames returns nullopt");
    check(!std::filesystem::exists(destination), "a failed reverse leaves no partial file behind");

    std::filesystem::remove(empty);
    std::filesystem::remove(source);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);

    test_reverses_a_span();
    test_reverses_a_sub_span();
    test_a_missing_source_fails_cleanly();
    test_a_failed_reverse_leaves_no_partial_file();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
