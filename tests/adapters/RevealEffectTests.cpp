// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <array>
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
#include <system_error>
#include <vector>

#include "adapters/engine/ges/effects/RevealEffect.h"
#include "core/ShaderPass.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

// Headless, no-framework: a self-created EGL context the way the other adapter
// suites do (TextureMixTests.cpp, RevealBindTests.cpp). This suite drives the
// reusable reveal effect end-to-end: a real GESClip with the reveal as a top
// effect, played through a GESPipeline to EOS, with the effect's own output
// captured for pixel assertions. The decisive proofs are (1) the reveal map
// independently masks the picture - green where an ordered region has passed
// `progress`, transparent black where it has not - and (2) the picture's alpha
// survives the effect (opaque where revealed, 0 where hidden), and (3) a flush
// seek forward and back still renders the right picture (the map stays
// resident across the seek).

namespace {

using genesis::adapters::engine::ges::install_reveal_effect;
using genesis::adapters::engine::ges::kRevealFilterId;
using genesis::adapters::engine::ges::make_reveal_effect;
using genesis::adapters::engine::ges::register_reveal_effect;
using genesis::adapters::engine::ges::update_reveal_progress;
using genesis::core::RevealMap;
using genesis::core::ShaderPass;

using genesis::test::check;

// The canvas: 96x64, three horizontal thirds ordering left (0), middle (128),
// right (255). The middle byte 128 normalises to 0.50196, so the body's
// `order <= progress` boundary at 0.5 keeps the middle hidden - the threshold
// is exact, and 0.75 is the value that reveals it (see the mask table below).
constexpr std::uint32_t kW = 96;
constexpr std::uint32_t kH = 64;

// The texturemix-convention body, byte-for-byte the one RevealBindTests uses:
// reads the picture (tex0) and the map (tex1), keeps the picture only where the
// map's order has passed `progress`, and declares its own non-sampler uniform.
constexpr const char *kRevealBody =
        "uniform float progress;"
        " void main () { vec4 c = texture2D (tex0, v_texcoord);"
        " float order = texture2D (tex1, v_texcoord).r;"
        " gl_FragColor = (order <= progress) ? c : vec4 (0.0, 0.0, 0.0, 0.0); }";

// A pass carrying a three-region reveal map, with `progress` baked into the
// uniform block the same field/param layout the resolver produces. The map is
// `width`x`height`; its three vertical thirds order left (0), middle (128),
// right (255).
ShaderPass reveal_pass(float progress, std::uint32_t width = kW, std::uint32_t height = kH)
{
    ShaderPass pass;
    pass.package = "test.reveal";
    pass.key = "test.reveal@1#0000000000000000";
    pass.source = std::make_shared<const std::string>(kRevealBody);
    const std::int32_t third = static_cast<std::int32_t>(width / 3);
    const std::vector<std::array<std::int32_t, 4>> rects = {
        { 0, 0, third, static_cast<std::int32_t>(height) },
        { third, 0, static_cast<std::int32_t>(width) - 2 * third,
          static_cast<std::int32_t>(height) },
        { 2 * third, 0, static_cast<std::int32_t>(width) - 2 * third,
          static_cast<std::int32_t>(height) },
    };
    pass.reveal_map = RevealMap::from_rects(width, height, rects);
    pass.fields = { { "progress", "float", 0, 4 } };
    pass.params = std::vector<std::uint8_t>(ShaderPass::MIN_PARAMS, 0);
    std::memcpy(pass.params.data(), &progress, sizeof(progress));
    return pass;
}

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
// picture. The effect's output is forced to system-memory RGBA, so the capture
// carries the reveal's alpha (opaque where revealed, 0 where hidden) - the
// composited preview would have blended it over black and lost it.
struct CapturedFrame
{
    std::int64_t pts_ns = 0;
    std::vector<std::uint8_t> bytes;
};

// Every frame the effect's own output emits, in order, for pixel assertions.
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

// Per-run knobs for drive_reveal. The defaults reproduce the plain 0-inpoint /
// 0-start 2s clip the original assertions used; the inpoint test and the PNG
// capture override them.
struct DriveOptions
{
    double progress = 0.0;
    std::vector<std::int64_t> seek_to_ns;
    std::int64_t inpoint_ns = 0;
    std::int64_t start_ns = 0;
    std::int64_t duration_ns = 2 * GST_SECOND;
    std::uint32_t width = kW;
    std::uint32_t height = kH;
};

// A FLUSH seek does not change pipeline state, so `gst_element_get_state` after
// one returns immediately without waiting for nle's asynchronous recommit. In
// that window `current_stack_start` can exceed `segment->stop`, so a follow-up
// seek computes `start > stop` and nle rejects it (the `start <= stop`
// assertion). The recommit is signalled by ASYNC_DONE on the bus; nle posts one
// per async cycle (teardown of the old stack, then re-preroll of the new), so a
// single seek can post more than one. Block until the bus has been quiet of
// ASYNC_DONE for a grace period, or a bounded timeout elapses. Returns false on
// ERROR or when no ASYNC_DONE ever arrived, true once settled.
bool wait_for_async_settle(GstBus *bus, GstClockTime timeout_ns)
{
    const GstClockTime deadline = gst_util_get_timestamp() + timeout_ns;
    const GstClockTime quiet_ns = 250 * GST_MSECOND;
    bool seen_async_done = false;
    while (gst_util_get_timestamp() < deadline) {
        GstMessage *message = gst_bus_timed_pop_filtered(
                bus, quiet_ns,
                static_cast<GstMessageType>(GST_MESSAGE_ASYNC_DONE | GST_MESSAGE_ERROR));
        if (message == nullptr) {
            // Quiet for one grace period: settled once the seek's own
            // ASYNC_DONE has been observed; otherwise keep waiting for it.
            if (seen_async_done) {
                return true;
            }
            continue;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            gst_message_unref(message);
            return false;
        }
        seen_async_done = true;
        gst_message_unref(message);
    }
    return seen_async_done;
}

// Builds a timeline with one clip carrying the reveal effect, drives it to EOS,
// and returns the effect's captured RGBA output. The effect is built from
// `reveal_pass(0.0)` but `progress` is applied through update_reveal_progress so
// the re-upload path is what the assertions exercise. `start_ns`/`inpoint_ns`/
// `duration_ns` are the ges_layer_add_asset arguments (timeline position, source
// offset, clip length). When `seek_to_ns` is non-empty, the pipeline is
// prerolled to PAUSED and flush-seeked to each position (in order) before
// playing - the seek path the assertions for seek-correctness exercise.
RunResult drive_reveal(GlContext &gl, const std::string &source_path, const DriveOptions &options)
{
    RunResult result;

    GESBaseEffect *effect = make_reveal_effect(reveal_pass(0.0f, options.width, options.height));
    if (effect == nullptr) {
        result.error = true;
        result.error_message = "make_reveal_effect returned null";
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
    GESClip *clip = ges_layer_add_asset(layer, clip_asset, options.start_ns, options.inpoint_ns,
                                        options.duration_ns, GES_TRACK_TYPE_VIDEO);
    gst_object_unref(clip_asset);
    if (clip == nullptr || !install_reveal_effect(clip, effect)) {
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

    // Force the video track into system memory at the canvas size: without this
    // the VP8 decode stays in CUDA memory (NV12) and the effect's videoconvert
    // upload head cannot convert it. `video/x-raw` with no memory feature pins
    // system memory, so the picture arrives YUV/RGB and the effect uploads it.
    GstCaps *restriction = gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT,
                                               static_cast<gint>(options.width), "height",
                                               G_TYPE_INT, static_cast<gint>(options.height),
                                               "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
    ges_track_set_restriction_caps(video, restriction);
    gst_caps_unref(restriction);

    // The reveal knobs ride through the re-upload path, not the build-time bake.
    update_reveal_progress(effect, options.progress);

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
    // sync=FALSE lets the pipeline free-run rather than pace to the wall clock
    // (the ClipEffectTests capture-sink convention), so the EOS the source
    // posts at its 2s end is not held up waiting on a clock.
    GstElement *video_sink = gst_element_factory_make("fakesink", nullptr);
    GstElement *audio_sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(video_sink, "sync", FALSE, nullptr);
    g_object_set(audio_sink, "sync", FALSE, nullptr);
    ges_pipeline_preview_set_video_sink(pipeline, video_sink);
    ges_pipeline_preview_set_audio_sink(pipeline, audio_sink);

    GstBus *bus = gst_element_get_bus(GST_ELEMENT(pipeline));
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);

    if (!options.seek_to_ns.empty()) {
        // Preroll, then flush-seek to each position before rolling. The same
        // accurate flush seek the production session uses (GesSession::seek),
        // each followed by an ASYNC_DONE settle wait so a later seek never
        // arrives while nle is still recommitting the previous one. The stop is
        // bound to the clip end: seeking a clip that does not start at 0 leaves
        // the composition's segment stopped at the pre-clip gap, so an unbounded
        // seek (stop NONE) trips nle's `start <= stop` assertion when the target
        // lands inside the clip.
        if (gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PAUSED)
            == GST_STATE_CHANGE_FAILURE) {
            result.error = true;
            result.error_message = "pipeline failed to preroll";
        } else {
            GstState current = GST_STATE_VOID_PENDING;
            gst_element_get_state(GST_ELEMENT(pipeline), &current, nullptr, 5 * GST_SECOND);
            if (current != GST_STATE_PAUSED) {
                result.error = true;
                result.error_message = "pipeline did not reach PAUSED";
            }
            const GstClockTime seek_stop =
                    static_cast<GstClockTime>(options.start_ns + options.duration_ns);
            // Drain the preroll's own async messages so each seek's settle wait
            // below observes only that seek's recommit, never a stale ASYNC_DONE.
            GstMessage *stale = nullptr;
            while ((stale = gst_bus_pop(bus)) != nullptr) {
                gst_message_unref(stale);
            }
            for (const std::int64_t target : options.seek_to_ns) {
                const gboolean sought = gst_element_seek(
                        GST_ELEMENT(pipeline), 1.0, GST_FORMAT_TIME,
                        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                        GST_SEEK_TYPE_SET, static_cast<GstClockTime>(target), GST_SEEK_TYPE_SET,
                        seek_stop);
                if (!sought) {
                    result.error = true;
                    result.error_message = "flush seek rejected";
                    break;
                }
                if (!wait_for_async_settle(bus, 5 * GST_SECOND)) {
                    result.error = true;
                    result.error_message = "flush seek did not settle";
                    break;
                }
            }
            // The preroll frame(s) pushed while seeking belong to the pre-seek
            // positions; drop them so the assertions run against the post-seek
            // replay only.
            capture.frames.clear();
        }
    }

    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 15 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
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

// The three region sample points at mid-height: left, middle, right thirds.
// Revealed means the green picture with alpha 255; hidden means transparent
// black (0,0,0,0) - the alpha 0 is what distinguishes the reveal's hidden
// region from a composite over black. `width`/`height` default to the 96x64
// canvas the plain tests use; the 320x180 PNG capture passes its own.
void check_frame_pixels(const CapturedFrame &frame, bool left, bool middle, bool right,
                        const std::string &prefix, std::uint32_t width = kW,
                        std::uint32_t height = kH)
{
    const auto state = [](bool revealed) {
        return revealed ? std::pair<int, int>{ 255, 255 } : std::pair<int, int>{ 0, 0 };
    };
    const std::pair<int, int> l = state(left);
    const std::pair<int, int> m = state(middle);
    const std::pair<int, int> r = state(right);
    const int tolerance = 12;

    check(pixel_near(frame.bytes, width, width / 6, height / 2, 0, l.first, 0, l.second, tolerance),
          prefix + "left third " + (left ? "revealed green" : "hidden"));
    check(pixel_near(frame.bytes, width, width / 2, height / 2, 0, m.first, 0, m.second, tolerance),
          prefix + "middle third " + (middle ? "revealed green" : "hidden"));
    check(pixel_near(frame.bytes, width, 5 * width / 6, height / 2, 0, r.first, 0, r.second,
                     tolerance),
          prefix + "right third " + (right ? "revealed green" : "hidden"));
}

// One reveal run at a given progress, checked against the expected per-region
// state across the whole clip: the first frame (clip start) and the last frame
// (clip end) must both be right, which proves the static map stays resident for
// every picture frame rather than only the first. Returns the run so callers
// (the inpoint test) can also assert the captured PTS window.
RunResult check_reveal(GlContext &gl, const std::string &source, const DriveOptions &options,
                       bool left, bool middle, bool right)
{
    RunResult result = drive_reveal(gl, source, options);
    const std::string prefix = "progress " + std::to_string(options.progress) + ": ";

    check(!result.error,
          prefix + "render without error"
                  + (result.error ? " (" + result.error_message + ")" : ""));
    check(result.eos, prefix + "pipeline reaches EOS");
    check(!result.frames.empty(), prefix + "the reveal output captures");
    if (result.frames.empty()) {
        return result;
    }

    check_frame_pixels(result.frames.front(), left, middle, right, prefix + "first frame ",
                       options.width, options.height);
    check_frame_pixels(result.frames.back(), left, middle, right, prefix + "last frame ",
                       options.width, options.height);
    std::printf("  [%s] %zu frames, first pts=%lld last pts=%lld\n",
                std::to_string(options.progress).c_str(), result.frames.size(),
                static_cast<long long>(result.frames.front().pts_ns),
                static_cast<long long>(result.frames.back().pts_ns));
    return result;
}

// True when the reveal's captured output (a straight-alpha RGBA buffer whose
// hidden pixels are 0,0,0,0) is composited over black so a PNG shows hidden
// regions as black and revealed pixels as their colour.
std::vector<std::uint8_t> flatten_over_black(const std::vector<std::uint8_t> &rgba)
{
    std::vector<std::uint8_t> flat = rgba;
    for (std::size_t i = 0; i + 3 < flat.size(); i += 4) {
        const std::uint32_t a = flat[i + 3];
        flat[i] = static_cast<std::uint8_t>((flat[i] * a) / 255);
        flat[i + 1] = static_cast<std::uint8_t>((flat[i + 1] * a) / 255);
        flat[i + 2] = static_cast<std::uint8_t>((flat[i + 2] * a) / 255);
        flat[i + 3] = 255;
    }
    return flat;
}

// A flush seek forward then back, with the reveal at 0.75 (left + middle
// revealed, right hidden): after each seek the picture at the new position must
// still carry the right mask, proving the map survives the seek rather than
// going blank or dropping back to the picture. The seek targets are derived
// from the clip's [start, start+duration) window so the same routine drives both
// the plain clip and the nonzero-inpoint/start clip.
void check_seek(GlContext &gl, const std::string &source, const DriveOptions &options)
{
    const std::int64_t start = options.start_ns;
    const std::int64_t duration = options.duration_ns;
    const std::int64_t forward_target = start + duration / 2;
    const std::int64_t backward_late = start + 3 * duration / 4;
    const std::int64_t backward_early = start + duration / 4;

    // Forward: preroll at 0, seek to the clip midpoint, play. drive_reveal
    // clears the preroll frames after the seek, so every captured frame belongs
    // to the post-seek replay; the first and last of them must show the mask.
    DriveOptions forward = options;
    forward.seek_to_ns = { forward_target };
    RunResult fresult = drive_reveal(gl, source, forward);
    const std::string fprefix = "seek forward: ";
    check(!fresult.error,
          fprefix + "render without error"
                  + (fresult.error ? " (" + fresult.error_message + ")" : ""));
    check(fresult.eos, fprefix + "pipeline reaches EOS");
    check(!fresult.frames.empty(), fprefix + "the post-seek replay captures");
    if (!fresult.frames.empty()) {
        check_frame_pixels(fresult.frames.front(), true, true, false, fprefix, options.width,
                           options.height);
        check_frame_pixels(fresult.frames.back(), true, true, false, fprefix, options.width,
                           options.height);
        std::printf("  [seek forward] %zu frames, first pts=%lld last pts=%lld\n",
                    fresult.frames.size(), static_cast<long long>(fresult.frames.front().pts_ns),
                    static_cast<long long>(fresult.frames.back().pts_ns));
    }

    // Backward: preroll at 0, seek to the 3/4 point, then back to the 1/4
    // point, play. The post-seek replay (again preroll-free) must show the mask.
    DriveOptions backward = options;
    backward.seek_to_ns = { backward_late, backward_early };
    RunResult bresult = drive_reveal(gl, source, backward);
    const std::string bprefix = "seek backward: ";
    check(!bresult.error,
          bprefix + "render without error"
                  + (bresult.error ? " (" + bresult.error_message + ")" : ""));
    check(bresult.eos, bprefix + "pipeline reaches EOS");
    check(!bresult.frames.empty(), bprefix + "the post-seek replay captures");
    if (!bresult.frames.empty()) {
        check_frame_pixels(bresult.frames.front(), true, true, false, bprefix, options.width,
                           options.height);
        check_frame_pixels(bresult.frames.back(), true, true, false, bprefix, options.width,
                           options.height);
        std::printf("  [seek backward] %zu frames, first pts=%lld last pts=%lld\n",
                    bresult.frames.size(), static_cast<long long>(bresult.frames.front().pts_ns),
                    static_cast<long long>(bresult.frames.back().pts_ns));
    }
}

// A two-second green 96x64 VP8/WebM picture, generated once for the suite:
// long enough for a clip spanning two seconds and for a forward and a backward
// seek within it.
std::string source_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-reveal-source.webm";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc num-buffers=60 pattern=green ! "
                                "video/x-raw,width=96,height=64,framerate=30/1 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    const bool ok = std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
            && std::filesystem::file_size(generated) > 0;
    return ok ? generated.string() : std::string{ };
}

// A single-frame RGBA picture of three vertical thirds: red, green, blue. The
// reveal map's three regions line up with these thirds, so a reveal PNG shows
// exactly which bars are visible.
std::vector<std::uint8_t> color_bars_rgba(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height * 4);
    const std::uint32_t first = width / 3;
    const std::uint32_t second = 2 * width / 3;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t r = 0;
            std::uint8_t g = 0;
            std::uint8_t b = 0;
            if (x < first) {
                r = 255;
            } else if (x < second) {
                g = 255;
            } else {
                b = 255;
            }
            const std::size_t i = (static_cast<std::size_t>(y) * width + x) * 4;
            bytes[i] = r;
            bytes[i + 1] = g;
            bytes[i + 2] = b;
            bytes[i + 3] = 255;
        }
    }
    return bytes;
}

// Encodes one RGBA frame to PNG at `path` via the runtime pngenc element (no
// build/link change). Returns true when the file was written to EOS.
bool write_png(const std::string &path, const std::vector<std::uint8_t> &rgba, std::uint32_t width,
               std::uint32_t height)
{
    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *src = gst_element_factory_make("appsrc", nullptr);
    GstElement *enc = gst_element_factory_make("pngenc", nullptr);
    GstElement *sink = gst_element_factory_make("filesink", nullptr);
    bool ok = pipeline != nullptr && src != nullptr && enc != nullptr && sink != nullptr;
    if (ok) {
        g_object_set(sink, "location", path.c_str(), nullptr);
        GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGBA", "width",
                                            G_TYPE_INT, static_cast<gint>(width), "height",
                                            G_TYPE_INT, static_cast<gint>(height), "framerate",
                                            GST_TYPE_FRACTION, 1, 1, nullptr);
        g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, nullptr);
        gst_caps_unref(caps);
        gst_bin_add_many(GST_BIN(pipeline), src, enc, sink, nullptr);
        ok = gst_element_link_many(src, enc, sink, nullptr);
    }
    if (ok) {
        ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, rgba.size(), nullptr);
        GstMapInfo map_info;
        if (gst_buffer_map(buffer, &map_info, GST_MAP_WRITE)) {
            std::memcpy(map_info.data, rgba.data(), rgba.size());
            gst_buffer_unmap(buffer, &map_info);
        }
        GST_BUFFER_PTS(buffer) = 0;
        if (ok && gst_app_src_push_buffer(GST_APP_SRC(src), buffer) != GST_FLOW_OK) {
            ok = false;
        }
        if (ok) {
            gst_app_src_end_of_stream(GST_APP_SRC(src));
            GstBus *bus = gst_element_get_bus(pipeline);
            GstMessage *message = gst_bus_timed_pop_filtered(
                    bus, 10 * GST_SECOND,
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
    return ok;
}

// A two-second colour-bar VP8/WebM picture at `width`x`height`, generated with
// an appsrc push (the same red/green/blue thirds color_bars_rgba produces) so
// the reveal PNGs decode the exact same picture.
std::string color_bars_source_fixture(std::uint32_t width, std::uint32_t height)
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-reveal-colorbars.webm";
    std::filesystem::remove(generated);

    const std::vector<std::uint8_t> frame = color_bars_rgba(width, height);

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
                                            G_TYPE_INT, static_cast<gint>(width), "height",
                                            G_TYPE_INT, static_cast<gint>(height), "framerate",
                                            GST_TYPE_FRACTION, 30, 1, nullptr);
        g_object_set(src, "caps", caps, "format", GST_FORMAT_TIME, nullptr);
        gst_caps_unref(caps);
        gst_bin_add_many(GST_BIN(pipeline), src, convert, enc, mux, sink, nullptr);
        ok = gst_element_link_many(src, convert, enc, mux, sink, nullptr);
    }
    if (ok) {
        ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
        for (int i = 0; ok && i < 60; ++i) {
            GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame.size(), nullptr);
            GstMapInfo map_info;
            if (gst_buffer_map(buffer, &map_info, GST_MAP_WRITE)) {
                std::memcpy(map_info.data, frame.data(), frame.size());
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
                    bus, 10 * GST_SECOND,
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

// A reveal clip with a nonzero source inpoint AND a nonzero timeline start: the
// clip sits at 0.5s on the timeline and reads the source from 0.5s in, lasting
// 1s (0.5 + 1.0 = 1.5s, within the 2s source). The same mask table must hold at
// every progress, and a forward+back seek within the clip must survive - the
// inpoint/start must not shift the map or break the seek. The 1s length is
// asserted through the captured frame count: the effect's output PTS is
// clip-local (the source segment restarts at 0), so it cannot carry the
// timeline start.
void check_reveal_inpoint(GlContext &gl, const std::string &source)
{
    DriveOptions options;
    options.inpoint_ns = GST_SECOND / 2;
    options.start_ns = GST_SECOND / 2;
    options.duration_ns = GST_SECOND;

    const auto expect = [&](double progress, bool left, bool middle, bool right) {
        options.progress = progress;
        return check_reveal(gl, source, options, left, middle, right);
    };

    expect(0.0, true, false, false);
    expect(0.5, true, false, false);
    const RunResult at_075 = expect(0.75, true, true, false);
    expect(1.0, true, true, true);

    if (!at_075.frames.empty()) {
        // The 1s clip must play for one second, not the whole 2s source. The
        // effect's output PTS is clip-local (it restarts at 0), so the length
        // is asserted through the frame count: 1s at 30fps is 30 frames, but the
        // preroll-to-replay boundary pushes a few duplicate frames (3-4
        // observed), so the band allows up to five extras while still clearly
        // distinguishing the 1s clip from the full 2s source (60 frames).
        const std::size_t count = at_075.frames.size();
        const std::size_t expected =
                static_cast<std::size_t>(options.duration_ns * 30 / GST_SECOND);
        check(count + 3 >= expected && count <= expected + 5,
              "inpoint reveal: the 1s clip yields ~30 frames");
        std::printf("  [inpoint] %zu frames, first pts=%lld last pts=%lld\n", count,
                    static_cast<long long>(at_075.frames.front().pts_ns),
                    static_cast<long long>(at_075.frames.back().pts_ns));
    }

    options.progress = 0.75;
    check_seek(gl, source, options);
}

// Writes visual evidence to /tmp/opencode/reveal-visual: a baseline colour-bar
// PNG (no effect) plus one PNG per progress showing exactly which bars the
// reveal exposes at 320x180.
void emit_pngs(GlContext &gl)
{
    const std::uint32_t width = 320;
    const std::uint32_t height = 180;
    const std::filesystem::path dir = "/tmp/opencode/reveal-visual";
    std::filesystem::create_directories(dir);

    const std::vector<std::uint8_t> bars = color_bars_rgba(width, height);
    check(write_png((dir / "00_baseline.png").string(), bars, width, height),
          "baseline PNG (no effect) written");

    const std::string source = color_bars_source_fixture(width, height);
    check(!source.empty(), "a colour-bar source fixture is available");
    if (source.empty()) {
        return;
    }

    const struct Capture
    {
        double progress;
        const char *name;
    } captures[] = {
        { 0.0, "01_progress_0.0.png" },
        { 0.5, "02_progress_0.5.png" },
        { 0.75, "03_progress_0.75.png" },
        { 1.0, "04_progress_1.0.png" },
    };

    for (const Capture &capture : captures) {
        DriveOptions options;
        options.progress = capture.progress;
        options.width = width;
        options.height = height;
        const RunResult result = drive_reveal(gl, source, options);
        check(!result.error && result.eos && !result.frames.empty(),
              std::string("colour-bar reveal ") + capture.name + " captured");
        if (result.frames.empty()) {
            continue;
        }
        // The reveal's transparent-hidden output is composited over black so the
        // PNG shows hidden thirds as black and revealed bars as colour.
        const std::vector<std::uint8_t> flat = flatten_over_black(result.frames.front().bytes);
        check(write_png((dir / capture.name).string(), flat, width, height),
              std::string(capture.name) + " written");
    }
}

void run_gl_tests(GlContext &gl)
{
    const std::string source = source_fixture();
    check(!source.empty(), "a green source fixture is available");
    if (source.empty()) {
        return;
    }

    // The mask table for `order <= progress` with orders 0 / 0.502 / 1.0:
    //   progress 0.0  -> left only
    //   progress 0.5  -> left only (0.502 > 0.5; the boundary is exact)
    //   progress 0.75 -> left + middle
    //   progress 1.0  -> all three
    const auto progress_only = [](double progress) {
        DriveOptions options;
        options.progress = progress;
        return options;
    };
    check_reveal(gl, source, progress_only(0.0), true, false, false);
    check_reveal(gl, source, progress_only(0.5), true, false, false);
    check_reveal(gl, source, progress_only(0.75), true, true, false);
    check_reveal(gl, source, progress_only(1.0), true, true, true);

    DriveOptions seek_options;
    seek_options.progress = 0.75;
    check_seek(gl, source, seek_options);

    check_reveal_inpoint(gl, source);
    emit_pngs(gl);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

    // No GL needed: the type registration is a pure GType check.
    check(register_reveal_effect(), "the reveal effect type registers");
    check(kRevealFilterId != nullptr && kRevealFilterId[0] != '\0',
          "the reveal filter id is non-empty");

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
