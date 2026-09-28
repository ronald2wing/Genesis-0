// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <ges/ges.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adapters/engine/ges/effects/MaskEffect.h"
#include "ai/vision/MaskStore.h"
#include "jobs/Job.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

// Headless, no-framework: a self-created EGL context the way the other adapter
// suites do (RevealEffectTests.cpp, TextureMixTests.cpp). This suite drives the
// reusable mask effect end-to-end: a real GESClip with the mask as a top effect,
// played through a GESPipeline to EOS, with the effect's own output captured for
// pixel assertions. The decisive proof is that the per-frame mask independently
// modulates the picture's alpha - a mask that cuts the left half and keeps the
// right yields a transparent left and a green right for every frame, which a
// static (imagefreeze) map could not do and a mask aliased to the picture would
// leave both halves green.

namespace {

using genesis::adapters::engine::ges::install_mask_effect;
using genesis::adapters::engine::ges::make_mask_effect;
using genesis::adapters::engine::ges::register_mask_effect;

using genesis::test::check;

// The render and fixture budgets are wall-clock ceilings on a GL pipeline that
// is far slower on a contended CI runner than on a dev host. Scale them the way
// the perf suite scales its budgets (tests/perf/BenchmarkTests.cpp): an explicit
// GENESIS_PERF_BUDGET_SCALE wins, otherwise `CI` implies 8x, otherwise 1.0. A
// non-positive override is a misconfiguration and is ignored.
double budget_scale()
{
    const char *override = std::getenv("GENESIS_PERF_BUDGET_SCALE");
    if (override != nullptr && *override != '\0') {
        const double value = std::atof(override);
        if (value > 0.0) {
            return value;
        }
    }
    if (std::getenv("CI") != nullptr) {
        return 8.0;
    }
    return 1.0;
}

// The canvas: 96x64, the left half cut (alpha 0) and the right half kept
// (alpha 255). Small so the source encodes fast and the mask files are tiny.
constexpr std::uint32_t kW = 96;
constexpr std::uint32_t kH = 64;
constexpr int kFrames = 60; // 2s at 30fps

// The headless EGL display + context the GL elements borrow, answering the
// NEED_CONTEXT messages the GL elements post before negotiation.
struct GlContext
{
    GstGLDisplay *display = nullptr;
    GstGLContext *context = nullptr;

    ~GlContext()
    {
        if (context != nullptr) {
            gst_object_unref(context);
        }
        if (display != nullptr) {
            gst_object_unref(display);
        }
    }
};

GstBusSyncReply on_bus_sync(GstBus *bus, GstMessage *message, gpointer user_data)
{
    (void)bus;
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_NEED_CONTEXT) {
        return GST_BUS_PASS;
    }
    auto *gl = static_cast<GlContext *>(user_data);

    const gchar *context_type = nullptr;
    gst_message_parse_context_type(message, &context_type);
    if (g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
        GstContext *context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(context, gl->display);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context);
        gst_context_unref(context);
        return GST_BUS_DROP;
    }
    if (g_strcmp0(context_type, "gst.gl.app_context") == 0) {
        GstContext *context = gst_context_new("gst.gl.app_context", TRUE);
        GstStructure *structure = gst_context_writable_structure(context);
        gst_structure_set(structure, "context", GST_TYPE_GL_CONTEXT, gl->context, nullptr);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context);
        gst_context_unref(context);
        return GST_BUS_DROP;
    }
    return GST_BUS_PASS;
}

// One in-flight asset request, completed synchronously on a GMainLoop (the sync
// ges_asset_request returns null for an uncached asset, so the async form plus
// a loop is how the clip asset actually loads).
struct AssetRequest
{
    GMainLoop *loop = nullptr;
    GESAsset *asset = nullptr;
    GError *error = nullptr;
    bool timed_out = false;
};

void on_asset_ready(GObject *, GAsyncResult *result, gpointer user_data)
{
    auto *request = static_cast<AssetRequest *>(user_data);
    request->asset = ges_asset_request_finish(result, &request->error);
    g_main_loop_quit(request->loop);
}

gboolean on_asset_timeout(gpointer user_data)
{
    auto *request = static_cast<AssetRequest *>(user_data);
    request->timed_out = true;
    g_main_loop_quit(request->loop);
    return G_SOURCE_REMOVE;
}

GESAsset *request_asset(const gchar *uri)
{
    AssetRequest request;
    request.loop = g_main_loop_new(nullptr, FALSE);
    ges_asset_request_async(GES_TYPE_URI_CLIP, uri, nullptr, on_asset_ready, &request);
    // Bound the wait: a missing media file or a GES failure would otherwise
    // block here until ctest's 1500 s default kills the suite.
    const guint timeout_id = g_timeout_add(30000, on_asset_timeout, &request);
    g_main_loop_run(request.loop);
    if (!request.timed_out) {
        g_source_remove(timeout_id);
    }
    g_main_loop_unref(request.loop);
    if (request.timed_out) {
        std::fprintf(stderr, "request_asset: timed out waiting for %s\n", uri);
        return nullptr;
    }
    if (request.error != nullptr) {
        g_error_free(request.error);
        return nullptr;
    }
    return request.asset;
}

// One captured effect-output frame: the source-media-time PTS and the full RGBA
// picture, so the mask's alpha survives into the capture (the composited
// preview would have blended it over black and lost it).
struct CapturedFrame
{
    std::int64_t pts_ns = 0;
    std::vector<std::uint8_t> bytes;
};

struct CaptureLog
{
    std::vector<CapturedFrame> frames;
};

GstPadProbeReturn on_capture(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    (void)pad;
    if ((info->type & GST_PAD_PROBE_TYPE_BUFFER) == 0) {
        return GST_PAD_PROBE_OK;
    }
    auto *log = static_cast<CaptureLog *>(user_data);
    GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstMapInfo map_info;
    if (gst_buffer_map(buffer, &map_info, GST_MAP_READ)) {
        CapturedFrame frame;
        frame.pts_ns = GST_BUFFER_PTS_IS_VALID(buffer)
                ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer))
                : 0;
        frame.bytes.assign(map_info.data, map_info.data + map_info.size);
        log->frames.push_back(std::move(frame));
        gst_buffer_unmap(buffer, &map_info);
    }
    return GST_PAD_PROBE_OK;
}

struct RunResult
{
    bool eos = false;
    bool error = false;
    std::string error_message;
    std::vector<CapturedFrame> frames;
};

// Builds a timeline with one clip carrying the mask effect, drives it to EOS,
// and returns the effect's captured RGBA output.
RunResult drive_mask(GlContext &gl, const std::string &source_path,
                     const genesis::ai::vision::MaskSpec &spec)
{
    RunResult result;

    GESBaseEffect *effect = make_mask_effect(spec, genesis::core::FrameRate::THIRTY);
    if (effect == nullptr) {
        result.error = true;
        result.error_message = "make_mask_effect returned null";
        return result;
    }

    GESTimeline *timeline = ges_timeline_new();
    GESLayer *layer = ges_timeline_append_layer(timeline);

    gchar *uri = gst_filename_to_uri(source_path.c_str(), nullptr);
    GESAsset *clip_asset = uri != nullptr ? request_asset(uri) : nullptr;
    g_free(uri);
    if (clip_asset == nullptr) {
        gst_object_unref(timeline);
        gst_object_unref(effect);
        result.error = true;
        result.error_message = "clip asset request failed";
        return result;
    }
    GESClip *clip =
            ges_layer_add_asset(layer, clip_asset, 0, 0, 2 * GST_SECOND, GES_TRACK_TYPE_VIDEO);
    gst_object_unref(clip_asset);
    if (clip == nullptr || !install_mask_effect(clip, effect)) {
        gst_object_unref(timeline);
        gst_object_unref(effect);
        result.error = true;
        result.error_message = "clip or effect install failed";
        return result;
    }

    GESTrack *video = GES_TRACK(ges_video_track_new());
    GESTrack *audio = GES_TRACK(ges_audio_track_new());
    ges_timeline_add_track(timeline, video);
    ges_timeline_add_track(timeline, audio);

    // Force the video track into system memory at the mask size: the VP8 decode
    // must arrive at the effect's videoconvert head in system memory, and at the
    // mask's dimensions so the two texturemix sinks negotiate a common size.
    GstCaps *restriction = gst_caps_new_simple(
            "video/x-raw", "width", G_TYPE_INT, static_cast<gint>(spec.width), "height", G_TYPE_INT,
            static_cast<gint>(spec.height), "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
    ges_track_set_restriction_caps(video, restriction);
    gst_caps_unref(restriction);

    // Capture the effect's own output (RGBA with alpha) before the compositor.
    CaptureLog capture;
    GstElement *bin = ges_track_element_get_element(GES_TRACK_ELEMENT(effect));
    GstPad *src = bin != nullptr ? gst_element_get_static_pad(bin, "src") : nullptr;
    if (src == nullptr) {
        gst_object_unref(timeline);
        result.error = true;
        result.error_message = "effect src pad missing";
        return result;
    }
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, on_capture, &capture, nullptr);
    gst_object_unref(src);

    GESPipeline *pipeline = ges_pipeline_new();
    gst_object_ref_sink(timeline);
    if (!ges_pipeline_set_timeline(pipeline, timeline)) {
        gst_object_unref(pipeline);
        gst_object_unref(timeline);
        result.error = true;
        result.error_message = "pipeline rejected the timeline";
        return result;
    }
    gst_object_unref(timeline);
    GstElement *video_sink = gst_element_factory_make("fakesink", nullptr);
    GstElement *audio_sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(video_sink, "sync", FALSE, nullptr);
    g_object_set(audio_sink, "sync", FALSE, nullptr);
    ges_pipeline_preview_set_video_sink(pipeline, video_sink);
    ges_pipeline_preview_set_audio_sink(pipeline, audio_sink);

    GstBus *bus = gst_element_get_bus(GST_ELEMENT(pipeline));
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);

    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
            bus, static_cast<GstClockTime>(15 * GST_SECOND * budget_scale()),
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (message != nullptr) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            result.error = true;
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            result.error_message = error != nullptr ? error->message : "unknown render error";
            g_clear_error(&error);
            g_free(debug);
        } else {
            result.eos = true;
        }
        gst_message_unref(message);
    } else {
        result.error = true;
        result.error_message = "no EOS or error (timeout)";
    }

    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    result.frames = std::move(capture.frames);
    return result;
}

// True when the pixel at (x, y) of a width*height RGBA buffer is within
// `tolerance` of (r, g, b) with alpha within `tolerance` of `a`.
bool pixel_near(const std::vector<std::uint8_t> &bytes, std::uint32_t width, std::uint32_t x,
                std::uint32_t y, int r, int g, int b, int a, int tolerance)
{
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
    if (index + 3 >= bytes.size()) {
        return false;
    }
    return std::abs(static_cast<int>(bytes[index]) - r) <= tolerance
            && std::abs(static_cast<int>(bytes[index + 1]) - g) <= tolerance
            && std::abs(static_cast<int>(bytes[index + 2]) - b) <= tolerance
            && std::abs(static_cast<int>(bytes[index + 3]) - a) <= tolerance;
}

// The split mask: left half cut (alpha 0), right half kept (alpha 255).
genesis::ai::vision::MaskData split_mask()
{
    genesis::ai::vision::MaskData mask;
    mask.width = kW;
    mask.height = kH;
    mask.alpha.resize(static_cast<std::size_t>(kW) * kH);
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            mask.alpha[static_cast<std::size_t>(y) * kW + x] = x < kW / 2 ? 0 : 255;
        }
    }
    return mask;
}

// A two-second green VP8/WebM picture, generated with an appsrc push so the
// suite needs no external gst-launch binary. Returns an empty string on failure.
std::string source_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-mask-source.webm";
    std::filesystem::remove(generated);

    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *src = gst_element_factory_make("appsrc", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *enc = gst_element_factory_make("vp8enc", nullptr);
    GstElement *mux = gst_element_factory_make("webmmux", nullptr);
    GstElement *sink = gst_element_factory_make("filesink", nullptr);
    bool ok = pipeline != nullptr && src != nullptr && convert != nullptr && enc != nullptr
            && mux != nullptr && sink != nullptr;
    if (ok) {
        g_object_set(sink, "location", generated.string().c_str(), nullptr);
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                            G_TYPE_INT, static_cast<gint>(kW), "height", G_TYPE_INT,
                                            static_cast<gint>(kH), "framerate", GST_TYPE_FRACTION,
                                            30, 1, nullptr);
        g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, nullptr);
        gst_caps_unref(caps);
        gst_bin_add_many(GST_BIN(pipeline), src, convert, enc, mux, sink, nullptr);
        ok = gst_element_link_many(src, convert, enc, mux, sink, nullptr);
    }
    if (ok) {
        ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
        const std::size_t frame_bytes = static_cast<std::size_t>(kW) * kH * 4;
        std::vector<std::uint8_t> green(frame_bytes, 0);
        for (std::size_t i = 1; i < frame_bytes; i += 4) {
            green[i] = 255; // G channel
        }
        for (std::size_t i = 3; i < frame_bytes; i += 4) {
            green[i] = 255; // A channel
        }
        for (int i = 0; ok && i < kFrames; ++i) {
            GstBuffer *buffer = gst_buffer_new_allocate(nullptr, green.size(), nullptr);
            GstMapInfo map_info;
            if (gst_buffer_map(buffer, &map_info, GST_MAP_WRITE)) {
                std::memcpy(map_info.data, green.data(), green.size());
                gst_buffer_unmap(buffer, &map_info);
            }
            GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(i) * GST_SECOND / 30;
            GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
            if (gst_app_src_push_buffer(GST_APP_SRC(src), buffer) != GST_FLOW_OK) {
                ok = false;
            }
        }
        if (ok) {
            gst_app_src_end_of_stream(GST_APP_SRC(src));
            GstBus *bus = gst_element_get_bus(pipeline);
            GstMessage *message = gst_bus_timed_pop_filtered(
                    bus, static_cast<GstClockTime>(10 * GST_SECOND * budget_scale()),
                    static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
            ok = message != nullptr && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
            if (message != nullptr) {
                gst_message_unref(message);
            }
            gst_object_unref(bus);
        }
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    ok = ok && std::filesystem::exists(generated) && std::filesystem::file_size(generated) > 0;
    return ok ? generated.string() : std::string{ };
}

void test_make_refuses_zero_size()
{
    genesis::ai::vision::MaskSpec spec;
    spec.width = 0;
    spec.height = 0;
    check(make_mask_effect(spec, genesis::core::FrameRate::THIRTY) == nullptr,
          "a zero-sized spec builds no effect");
}

void run_gl_tests(GlContext &gl)
{
    const std::string source = source_fixture();
    check(!source.empty(), "a green source fixture is available");
    if (source.empty()) {
        return;
    }

    // The mask spec: keyed by the source at its native size, one mask per frame.
    const std::filesystem::path root =
            std::filesystem::temp_directory_path() / "genesis-mask-tests";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    genesis::ai::vision::MaskSpec spec;
    spec.root = root;
    spec.source = source;
    spec.source_size = std::filesystem::file_size(source);
    spec.subject = genesis::jobs::Subject::Person;
    spec.model_id = "rvm";
    spec.width = kW;
    spec.height = kH;
    spec.stride = 1;

    // One split mask per source frame (plus a few past the end), so every frame
    // the probe reads is cut left / kept right. The probe looks each frame up
    // through mask_path_for, which is the same naming the driver writes.
    const genesis::ai::vision::MaskData mask = split_mask();
    for (std::uint32_t frame = 0; frame <= kFrames + 4; ++frame) {
        const std::filesystem::path path = genesis::ai::vision::mask_path_for(spec, frame);
        check(genesis::ai::vision::write_mask(path, mask),
              "a split mask writes for frame " + std::to_string(frame));
    }

    RunResult result = drive_mask(gl, source, spec);
    check(!result.error,
          "mask render without error" + (result.error ? " (" + result.error_message + ")" : ""));
    check(result.eos, "mask pipeline reaches EOS");
    check(!result.frames.empty(), "the mask output captures");
    if (result.frames.empty()) {
        std::filesystem::remove_all(root);
        return;
    }

    const int tolerance = 12;
    const auto check_frame = [&](const CapturedFrame &frame, const std::string &prefix) {
        check(pixel_near(frame.bytes, kW, kW / 4, kH / 2, 0, 0, 0, 0, tolerance),
              prefix + "left half is cut (alpha 0)");
        check(pixel_near(frame.bytes, kW, 3 * kW / 4, kH / 2, 0, 255, 0, 255, tolerance),
              prefix + "right half is the green picture");
    };

    check_frame(result.frames.front(), "first frame: ");
    check_frame(result.frames.back(), "last frame: ");

    std::filesystem::remove_all(root);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    // No GL needed: the type registration is a pure GType check.
    check(register_mask_effect(), "the mask effect type registers");
    test_make_refuses_zero_size();

    GlContext gl;
    gl.display = genesis::test::make_egl_display();
    check(gl.display != nullptr, "a headless EGL display is available");
    if (gl.display != nullptr) {
        gl.context = gst_gl_context_new(gl.display);
        GError *error = nullptr;
        if (!gst_gl_context_create(gl.context, nullptr, &error)) {
            check(false,
                  std::string("a headless GL context creates: ")
                          + (error != nullptr ? error->message : "unknown"));
            g_clear_error(&error);
        } else {
            run_gl_tests(gl);
        }
    }

    return genesis::test::summary();
}
