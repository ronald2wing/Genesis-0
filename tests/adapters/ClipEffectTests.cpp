// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <ges/ges.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>

#include "adapters/engine/ges/effects/ClipEffects.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "project/model/Clip.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "render/Catalogue.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

// Headless, no-framework: proves the per-clip top-effect path
// (install_clip_effects / configure_clip_effects) really renders, by driving a
// hand-built GES timeline - one URI clip plus top effects - through a
// GESPipeline in preview mode and capturing the RGBA output of a custom video
// sink. The GL context is supplied the way the other adapter suites do
// (FrameTextureTests.cpp, TextureMixTests.cpp).

namespace {

using genesis::test::check;

using namespace genesis::project;
namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::core::Rational;
using genesis::core::ShaderPass;
using ges_adapter::ClipEffectChain;

// The exact rational -> nanosecond seam the builder uses, so clip times here
// match what install_clip_effects derives from the host clip.
std::int64_t to_ns(Rational time)
{
    return static_cast<std::int64_t>(std::llround(time.as_double() * GST_SECOND));
}

// The media file every render reads: a solid green VP8/WebM, the only encoder
// guaranteed on this machine. Green is the load-bearing choice - invert turns
// it magenta, and exposure dims its G channel - so every assertion is a
// channel-dominance comparison robust to the codec's small noise. Returns an
// empty path when generation fails, and the test then fails loudly.
struct MediaFixture
{
    std::string path;
    bool owns_file = false;
};

MediaFixture media_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-clip-effect.webm";
    std::filesystem::remove(generated);
    // 120 frames at 30 fps = four seconds, enough for a clip reading at a
    // two-second inpoint. Pinned to 320x180 so captures stay small.
    const std::string command = "gst-launch-1.0 -q videotestsrc pattern=green num-buffers=120 ! "
                                "video/x-raw,width=320,height=180,framerate=30/1 ! videoconvert ! "
                                "vp8enc ! webmmux ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return MediaFixture{ generated.string(), true };
    }
    std::filesystem::remove(generated);
    return MediaFixture{ "", false };
}

// The headless EGL display + context the GL effect elements borrow, answering
// the NEED_CONTEXT messages the glupload/glshader/gldownload chain posts.
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

// The g_warning messages install_clip_effects emits for skipped links, so the
// unknown-pack case can assert the diagnostic rather than just the omission.
std::vector<std::string> g_log_warnings;

void on_glib_log(const gchar *domain, GLogLevelFlags level, const gchar *message,
                 gpointer user_data)
{
    (void)domain;
    (void)level;
    auto *out = static_cast<std::vector<std::string> *>(user_data);
    out->push_back(message != nullptr ? message : "");
}

// How many glshader elements the timeline carries, reached the same way the
// builder reaches them: each clip's track elements' element, into the effect
// bin, counting glshaders recursively.
std::size_t count_glshaders_in_bin(GstBin *bin)
{
    std::size_t count = 0;
    GstIterator *iterator = gst_bin_iterate_recurse(bin);
    GValue item = G_VALUE_INIT;
    bool done = false;
    while (!done) {
        switch (gst_iterator_next(iterator, &item)) {
        case GST_ITERATOR_OK: {
            GstElement *element = GST_ELEMENT(g_value_get_object(&item));
            GstElementFactory *factory = gst_element_get_factory(element);
            if (factory != nullptr
                && g_strcmp0(GST_OBJECT_NAME(GST_OBJECT(factory)), "glshader") == 0) {
                ++count;
            }
            g_value_reset(&item);
            break;
        }
        case GST_ITERATOR_RESYNC:
            gst_iterator_resync(iterator);
            break;
        case GST_ITERATOR_ERROR:
        case GST_ITERATOR_DONE:
            done = true;
            break;
        }
    }
    g_value_unset(&item);
    gst_iterator_free(iterator);
    return count;
}

std::size_t count_glshaders(GESTimeline *timeline)
{
    std::size_t count = 0;
    GList *layers = ges_timeline_get_layers(timeline);
    for (GList *layer_it = layers; layer_it != nullptr; layer_it = layer_it->next) {
        GList *clips = ges_layer_get_clips(GES_LAYER(layer_it->data));
        for (GList *clip_it = clips; clip_it != nullptr; clip_it = clip_it->next) {
            GList *children = ges_container_get_children(GES_CONTAINER(clip_it->data), TRUE);
            for (GList *child_it = children; child_it != nullptr; child_it = child_it->next) {
                GstElement *element =
                        ges_track_element_get_element(GES_TRACK_ELEMENT(child_it->data));
                if (element != nullptr && GST_IS_BIN(element)) {
                    count += count_glshaders_in_bin(GST_BIN(element));
                }
            }
            g_list_free(children);
        }
        g_list_free(clips);
    }
    g_list_free(layers);
    return count;
}

// One captured frame: the timeline-time PTS and the RGBA centre pixel.
struct RenderedFrame
{
    std::int64_t pts_ns = 0;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// What the capture sink records, written by the streaming thread and read
// after teardown.
struct CaptureLog
{
    std::vector<RenderedFrame> frames;
};

GstFlowReturn on_capture_sample(GstAppSink *sink, gpointer user_data)
{
    auto *log = static_cast<CaptureLog *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (buffer != nullptr && caps != nullptr) {
        GstStructure *structure = gst_caps_get_structure(caps, 0);
        gint width = 0;
        gint height = 0;
        gst_structure_get_int(structure, "width", &width);
        gst_structure_get_int(structure, "height", &height);
        GstMapInfo info;
        if (width > 0 && height > 0 && gst_buffer_map(buffer, &info, GST_MAP_READ)) {
            const std::size_t index =
                    (static_cast<std::size_t>(height / 2) * width + width / 2) * 4;
            if (index + 3 < info.size) {
                RenderedFrame frame;
                frame.pts_ns = GST_BUFFER_PTS_IS_VALID(buffer)
                        ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer))
                        : 0;
                frame.r = info.data[index];
                frame.g = info.data[index + 1];
                frame.b = info.data[index + 2];
                log->frames.push_back(frame);
            }
            gst_buffer_unmap(buffer, &info);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// The outcome of one render case: whether it reached EOS, the captured frames,
// and the structural facts gathered while the chain was still alive.
struct CaseResult
{
    bool eos = false;
    bool captured = false;
    std::string error;
    std::vector<RenderedFrame> frames;
    bool installed = false;
    bool configured = false;
    bool animated = false;
    int top_effect_count = -1;
    std::size_t glshader_count = 0;
    std::vector<std::string> install_warnings;
};

// The synchronous asset request GES needs: ges_asset_request returns null for
// an uncached asset, so the async form plus a loop is how an asset loads. The
// loop, asset, and error travel together in one struct - the user_data the
// callback both reads from and writes to (the same pattern the capture probes
// proved out).
GESAsset *request_asset(const gchar *uri)
{
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    struct Req
    {
        GMainLoop *loop;
        GESAsset *asset;
        GError *error;
        bool timed_out;
    };
    Req request{ loop, nullptr, nullptr, false };
    ges_asset_request_async(
            GES_TYPE_URI_CLIP, uri, nullptr,
            [](GObject *, GAsyncResult *result, gpointer user_data) {
                auto *request = static_cast<Req *>(user_data);
                request->asset = ges_asset_request_finish(result, &request->error);
                g_main_loop_quit(request->loop);
            },
            &request);
    // Bound the wait: a missing media file or a GES failure would otherwise
    // block here until ctest's 1500 s default kills the suite.
    const guint timeout_id = g_timeout_add(
            30000,
            [](gpointer user_data) -> gboolean {
                auto *request = static_cast<Req *>(user_data);
                request->timed_out = true;
                g_main_loop_quit(request->loop);
                return G_SOURCE_REMOVE;
            },
            &request);
    g_main_loop_run(loop);
    if (!request.timed_out) {
        g_source_remove(timeout_id);
    }
    g_main_loop_unref(loop);
    if (request.timed_out) {
        std::fprintf(stderr, "request_asset: timed out waiting for %s\n", uri);
        return nullptr;
    }
    if (request.error != nullptr) {
        g_error_free(request.error);
    }
    return request.asset;
}

// Builds one clip with `effects` over the green source at
// `[start, start+duration)` reading from `source_start`, installs and
// configures the clip effects, renders it, and captures the output. The chain
// is heap-allocated: configure transfers ownership to the probe's destroy
// notify on success, so the caller must not touch it after teardown.
CaseResult run_case(GlContext &gl, const std::string &media_path, Rational start,
                    Rational source_start, Rational duration,
                    const std::vector<AppliedFilter> &effects,
                    std::function<bool(const std::string &)> available = { })
{
    CaseResult result;

    GESTimeline *timeline = ges_timeline_new();
    GESLayer *layer = ges_timeline_append_layer(timeline);
    if (timeline == nullptr || layer == nullptr) {
        gst_clear_object(&timeline);
        return result;
    }

    gchar *uri = gst_filename_to_uri(media_path.c_str(), nullptr);
    GESAsset *asset = uri != nullptr ? request_asset(uri) : nullptr;
    g_free(uri);
    if (asset == nullptr) {
        gst_object_unref(timeline);
        result.error = "asset did not load";
        return result;
    }
    GESClip *clip = ges_layer_add_asset(layer, asset, to_ns(start), to_ns(source_start),
                                        to_ns(duration), GES_TRACK_TYPE_VIDEO);
    gst_object_unref(asset);
    if (clip == nullptr) {
        gst_object_unref(timeline);
        result.error = "clip did not build";
        return result;
    }

    // The host clip the chain resolves against: its effects, source inpoint,
    // and timeline length - what install_clip_effects derives its fraction
    // seam from.
    genesis::project::Clip host;
    host.id = "c1";
    host.start = start;
    host.source_start = source_start;
    host.duration = duration;
    host.video_effects = effects;

    ClipEffectChain *chain = new ClipEffectChain();
    chain->clip = host;
    if (available) {
        chain->available = std::move(available);
    }

    const std::size_t warn_before = g_log_warnings.size();
    result.installed = ges_adapter::install_clip_effects(clip, chain);
    result.animated = chain->is_animated;
    for (std::size_t i = warn_before; i < g_log_warnings.size(); ++i) {
        result.install_warnings.push_back(g_log_warnings[i]);
    }

    // One video plus one audio track, so the timeline can preroll and render;
    // the video track is pinned to the source size so captures stay small.
    GESTrack *video = GES_TRACK(ges_video_track_new());
    GESTrack *audio = GES_TRACK(ges_audio_track_new());
    if (video == nullptr || audio == nullptr || !ges_timeline_add_track(timeline, video)
        || !ges_timeline_add_track(timeline, audio)) {
        // The timeline sinks whichever track it accepted; we hold no reference
        // to either after handing them over, so neither is cleared here (an
        // unref of an accepted track would double-free it at timeline teardown).
        delete chain;
        gst_object_unref(timeline);
        result.error = "tracks did not add";
        return result;
    }
    GstCaps *restriction =
            gst_caps_new_simple("video/x-raw", "width", G_TYPE_INT, 320, "height", G_TYPE_INT, 180,
                                "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
    ges_track_set_restriction_caps(video, restriction);
    gst_caps_unref(restriction);

    if (result.installed) {
        result.configured = ges_adapter::configure_clip_effects(clip, chain);
    }

    result.top_effect_count = g_list_length(ges_clip_get_top_effects(clip));
    result.glshader_count = count_glshaders(timeline);

    // The custom preview sink: videoconvert forces RGBA system memory, the
    // appsink captures it. sync=FALSE lets the pipeline free-run rather than
    // pace to the wall clock.
    GstElement *sink_bin = gst_bin_new("capture-sink");
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", "sink");
    if (sink_bin == nullptr || convert == nullptr || appsink == nullptr) {
        gst_clear_object(&sink_bin);
        gst_clear_object(&convert);
        gst_clear_object(&appsink);
        delete chain;
        gst_object_unref(timeline);
        result.error = "capture sink did not build";
        return result;
    }
    g_object_set(appsink, "sync", FALSE, nullptr);
    GstCaps *sink_caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(appsink, "caps", sink_caps, nullptr);
    gst_caps_unref(sink_caps);
    gst_bin_add_many(GST_BIN(sink_bin), convert, appsink, nullptr);
    if (!gst_element_link(convert, appsink)) {
        delete chain;
        gst_object_unref(timeline);
        gst_object_unref(sink_bin);
        result.error = "capture sink did not link";
        return result;
    }
    GstPad *pad = gst_element_get_static_pad(convert, "sink");
    GstPad *ghost = gst_ghost_pad_new("sink", pad);
    gst_object_unref(pad);
    gst_element_add_pad(sink_bin, ghost);

    CaptureLog capture;
    GstAppSinkCallbacks callbacks = { };
    callbacks.new_sample = on_capture_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, &capture, nullptr);

    GESPipeline *pipeline = ges_pipeline_new();
    if (pipeline == nullptr) {
        delete chain;
        gst_object_unref(timeline);
        gst_object_unref(sink_bin);
        result.error = "pipeline did not build";
        return result;
    }
    gst_object_ref_sink(timeline);
    if (!ges_pipeline_set_timeline(pipeline, timeline)) {
        gst_object_unref(pipeline);
        gst_object_unref(timeline);
        gst_object_unref(sink_bin);
        delete chain;
        result.error = "pipeline rejected the timeline";
        return result;
    }
    gst_object_unref(timeline);

    GstElement *audio_sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(audio_sink, "sync", FALSE, nullptr);
    ges_pipeline_preview_set_video_sink(pipeline, sink_bin);
    ges_pipeline_preview_set_audio_sink(pipeline, audio_sink);

    GstBus *bus = gst_element_get_bus(GST_ELEMENT(pipeline));
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);

    if (gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        result.error = "failed to start";
    } else {
        GstMessage *message = gst_bus_timed_pop_filtered(
                bus, 15 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message != nullptr) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError *gerror = nullptr;
                gst_message_parse_error(message, &gerror, nullptr);
                result.error = gerror != nullptr ? gerror->message : "render failed";
                g_clear_error(&gerror);
            } else {
                result.eos = true;
            }
            gst_message_unref(message);
        } else {
            result.error = "no EOS or error (timeout)";
        }
    }

    gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    result.frames = std::move(capture.frames);
    result.captured = !result.frames.empty();

    // Ownership: configure handed the chain to the probe (its destroy notify
    // frees it at teardown, above); a failed install or configure leaves it
    // ours to free.
    if (!result.configured) {
        delete chain;
    }
    return result;
}

// The frame whose PTS is nearest `pts_ns`.
const RenderedFrame *frame_nearest(const std::vector<RenderedFrame> &frames, std::int64_t pts_ns)
{
    const RenderedFrame *best = nullptr;
    std::int64_t best_dist = 0;
    for (const RenderedFrame &frame : frames) {
        const std::int64_t dist = std::llabs(frame.pts_ns - pts_ns);
        if (best == nullptr || dist < best_dist) {
            best = &frame;
            best_dist = dist;
        }
    }
    return best;
}

// The measured centre pixel, printed so the completion report can cite real
// values rather than only channel-dominance booleans.
void report(const char *label, const RenderedFrame &frame)
{
    std::printf("  [%s] rgb=(%u,%u,%u) pts=%lld\n", label, frame.r, frame.g, frame.b,
                static_cast<long long>(frame.pts_ns));
}

// ---- Pure-logic tests (no GES, no GL). ----

void test_clip_fraction()
{
    ClipEffectChain chain;
    chain.source_start_ns = 2 * GST_SECOND;
    chain.duration_ns = 1 * GST_SECOND;

    check(ges_adapter::clip_fraction(chain, 2 * GST_SECOND) == 0.0,
          "the inpoint maps to fraction 0");
    check(std::abs(ges_adapter::clip_fraction(chain, 2 * GST_SECOND + GST_SECOND / 2) - 0.5) < 1e-9,
          "the midpoint maps to fraction 0.5");
    check(ges_adapter::clip_fraction(chain, 3 * GST_SECOND) == 1.0,
          "the outpoint maps to fraction 1");
    check(ges_adapter::clip_fraction(chain, 0) == 0.0, "a PTS before the inpoint clamps to 0");
    check(ges_adapter::clip_fraction(chain, 10 * GST_SECOND) == 1.0,
          "a PTS past the outpoint clamps to 1");

    ClipEffectChain zero;
    check(ges_adapter::clip_fraction(zero, 1 * GST_SECOND) == 0.0,
          "a zero-duration chain is fraction 0");
}

// The buffer probe's update logic, clip-local: a keyed `stops` knob rides 0 ->
// -4 across the clip, so the same fraction is a no-op and a later one is a
// change. No glshaders are wired, so the upload loop is skipped - this drives
// the resolve-and-compare logic only, like the treatment probe test.
void test_update_clip_uniforms()
{
    AppliedFilter exposure = AppliedFilter::create("genesis.exposure");
    exposure.keys["stops"] = std::vector<ParamKey>{
        ParamKey{ 0.0, 0.0, KeyEase::linear() },
        ParamKey{ 1.0, -4.0, KeyEase::linear() },
    };
    genesis::project::Clip clip;
    clip.id = "c1";
    clip.video_effects = { std::move(exposure) };

    ClipEffectChain chain;
    chain.clip = clip;

    check(ges_adapter::update_clip_uniforms(chain, 0.0), "the first frame is a change");
    check(chain.last.size() == 1, "one pass resolves");
    {
        GstStructure *uniforms = ges_adapter::uniforms_for(chain.last[0]);
        gfloat stops = 0.0f;
        gst_structure_get(uniforms, "stops", G_TYPE_FLOAT, &stops, nullptr);
        check(std::abs(stops) < 1e-3f, "stops reads 0 at the start");
        gst_structure_free(uniforms);
    }

    check(!ges_adapter::update_clip_uniforms(chain, 0.0), "an unchanged frame is a no-op");

    check(ges_adapter::update_clip_uniforms(chain, 1.0),
          "a later frame with moved knobs is a change");
    {
        GstStructure *uniforms = ges_adapter::uniforms_for(chain.last[0]);
        gfloat stops = 0.0f;
        gst_structure_get(uniforms, "stops", G_TYPE_FLOAT, &stops, nullptr);
        check(std::abs(stops + 4.0f) < 1e-3f, "stops reads -4 at the end");
        gst_structure_free(uniforms);
    }
}

// install can retain a compacted successful subset when a middle pass's effect
// fails to build or add - say [pass0, pass2]. A later frame re-resolves the
// full chain [pass0, pass1, pass2]; a positional upload would pour pass1's
// uniforms into pass2's shader. The structural guard must reject the mismatch
// before touching any shader and keep the last valid uniforms.
void test_update_clip_uniforms_structural_guard()
{
    const genesis::render::Catalogue &catalogue = genesis::render::Catalogue::builtin();

    // A three-pass clip, as a runtime frame would resolve it.
    genesis::project::Clip clip;
    clip.id = "c1";
    clip.video_effects = { AppliedFilter::create("genesis.invert"),
                           AppliedFilter::create("genesis.sepia"),
                           AppliedFilter::create("genesis.exposure") };
    const std::vector<ShaderPass> full = catalogue.passes_for(clip, 0.0).passes;
    check(full.size() == 3, "the guard fixture resolves three passes");
    if (full.size() != 3) {
        return;
    }

    // The installed chain after the middle effect failed: pass0 and pass2, and
    // one slot per installed pass. The shaders are sentinels, not GL elements:
    // if the guard uploaded to them, g_object_set would dereference and crash,
    // so surviving this test is the proof it does not.
    ClipEffectChain gapped;
    gapped.clip = clip;
    gapped.last = { full[0], full[2] };
    gapped.slots.resize(2);
    gapped.slots[0].shader = reinterpret_cast<GstElement *>(0x1);
    gapped.slots[1].shader = reinterpret_cast<GstElement *>(0x2);
    const std::vector<ShaderPass> gapped_before = gapped.last;

    check(!ges_adapter::update_clip_uniforms(gapped, 0.0), "a count mismatch uploads nothing");
    check(gapped.last == gapped_before, "the installed passes are retained on a count mismatch");

    // Same count, a different shader at a position: the identity half of the
    // guard, which the count check alone would miss.
    genesis::project::Clip pair;
    pair.id = "c2";
    pair.video_effects = { AppliedFilter::create("genesis.invert"),
                           AppliedFilter::create("genesis.sepia") };
    const std::vector<ShaderPass> two = catalogue.passes_for(pair, 0.0).passes;
    check(two.size() == 2, "the identity fixture resolves two passes");
    if (two.size() != 2) {
        return;
    }

    ClipEffectChain reshaped;
    reshaped.clip = pair;
    reshaped.last = two;
    reshaped.last[1].key += "!different";
    reshaped.slots.resize(2);
    reshaped.slots[0].shader = reinterpret_cast<GstElement *>(0x3);
    reshaped.slots[1].shader = reinterpret_cast<GstElement *>(0x4);
    const std::vector<ShaderPass> reshaped_before = reshaped.last;

    check(!ges_adapter::update_clip_uniforms(reshaped, 0.0),
          "an identity mismatch uploads nothing");
    check(reshaped.last == reshaped_before,
          "the installed passes are retained on an identity mismatch");

    // An ordinary animated pass must still upload: the guard compares
    // structure, not the per-frame parameter values.
    genesis::project::Clip moving;
    moving.id = "c3";
    AppliedFilter exposure = AppliedFilter::create("genesis.exposure");
    exposure.keys["stops"] = std::vector<ParamKey>{
        ParamKey{ 0.0, 0.0, KeyEase::linear() },
        ParamKey{ 1.0, -4.0, KeyEase::linear() },
    };
    moving.video_effects = { std::move(exposure) };
    const std::vector<ShaderPass> seeded = catalogue.passes_for(moving, 0.0).passes;
    check(seeded.size() == 1, "the moving fixture resolves one pass");
    if (seeded.size() != 1) {
        return;
    }
    ClipEffectChain animated;
    animated.clip = moving;
    animated.last = seeded;
    // No slots, so the upload loop is skipped and this drives the
    // resolve-and-compare path only.
    check(ges_adapter::update_clip_uniforms(animated, 1.0),
          "a structurally identical frame with moved knobs still uploads");
}

// A static chain (its enabled effects carry no keyed parameter) is resolved
// once at install and must not re-resolve on the streaming thread. The flag is
// set manually here: install_clip_effects needs a live GES clip, and this is a
// pure-logic test of the short-circuit it seeds. `last` stays empty, proving
// the catalogue resolve was never reached.
void test_static_chain_short_circuits()
{
    ClipEffectChain chain;
    chain.is_animated = false;
    chain.clip.id = "c1";
    chain.clip.video_effects = { AppliedFilter::create("genesis.invert") };

    check(!ges_adapter::update_clip_uniforms(chain, 0.5),
          "a static chain short-circuits to unchanged");
    check(chain.last.empty(), "a static chain never resolves, so nothing is uploaded");
}

// ---- Render tests. ----

void test_baseline_unchanged(GlContext &gl, const std::string &media_path)
{
    const CaseResult result =
            run_case(gl, media_path, Rational{ 0, 1 }, Rational{ 0, 1 }, Rational{ 1, 1 }, { });

    check(result.eos, "the baseline render reaches EOS");
    check(!result.installed, "no effects installs nothing");
    check(result.glshader_count == 0, "no effects adds no glshader");
    if (!result.captured || result.frames.empty()) {
        return;
    }
    const RenderedFrame first = result.frames.front();
    check(first.g > first.r + 100 && first.g > first.b + 100,
          "the picture stays green without effects");
    report("baseline green", first);
}

void test_invert_turns_green_magenta(GlContext &gl, const std::string &media_path)
{
    const CaseResult result =
            run_case(gl, media_path, Rational{ 0, 1 }, Rational{ 0, 1 }, Rational{ 1, 1 },
                     { AppliedFilter::create("genesis.invert") });

    check(result.eos, "the invert render reaches EOS");
    check(result.installed, "invert installs one effect");
    check(result.configured, "invert configures its shader");
    check(!result.animated, "a keyless invert is classified static");
    check(result.top_effect_count == 1, "one top effect is on the clip");
    check(result.glshader_count == 1, "one glshader for the one pass");
    if (!result.captured || result.frames.empty()) {
        return;
    }
    const RenderedFrame first = result.frames.front();
    check(first.r > first.g + 100 && first.b > first.g + 100,
          "invert turns green magenta (R and B high, G low)");
    report("invert(green)", first);
}

// Two noncommuting passes: invert then sepia gives a warm brown, sepia then
// invert a cool blue. The order in which the two top effects run is therefore
// observable in the pixels - the reverse-add in install_clip_effects must
// leave the first pass applied first.
void test_noncommuting_order(GlContext &gl, const std::string &media_path)
{
    const CaseResult invert_first = run_case(
            gl, media_path, Rational{ 0, 1 }, Rational{ 0, 1 }, Rational{ 1, 1 },
            { AppliedFilter::create("genesis.invert"), AppliedFilter::create("genesis.sepia") });
    const CaseResult sepia_first = run_case(
            gl, media_path, Rational{ 0, 1 }, Rational{ 0, 1 }, Rational{ 1, 1 },
            { AppliedFilter::create("genesis.sepia"), AppliedFilter::create("genesis.invert") });

    check(invert_first.eos && sepia_first.eos, "both two-pass renders reach EOS");
    check(invert_first.top_effect_count == 2 && sepia_first.top_effect_count == 2,
          "two top effects land in each clip");
    if (!invert_first.captured || invert_first.frames.empty() || !sepia_first.captured
        || sepia_first.frames.empty()) {
        return;
    }
    const RenderedFrame a = invert_first.frames.front();
    const RenderedFrame b = sepia_first.frames.front();
    report("invert-then-sepia", a);
    report("sepia-then-invert", b);

    // invert then sepia: green -> magenta -> sepia(magenta) = warm, R > G > B.
    check(a.r > a.g && a.g > a.b, "invert-then-sepia is warm (R > G > B)");
    // sepia then invert: green -> sepia(green) -> invert = cool, B > G > R.
    check(b.b > b.g && b.g > b.r, "sepia-then-invert is cool (B > G > R)");
    check(a.r > a.b && b.b > b.r, "the two orders produce different pictures");
}

// Animated exposure, with a NONZERO source inpoint and a NONZERO timeline
// start: the clip starts at 1s on the timeline and reads from 2s in the
// source, so the clip-local fraction - what the probe must feed the keyed
// knob - is only right if the PTS is source media time and the source inpoint
// is subtracted. A wrong absolute-time fraction clamps to 0 or 1 and the
// brightness stops moving.
void test_animated_exposure(GlContext &gl, const std::string &media_path)
{
    AppliedFilter exposure = AppliedFilter::create("genesis.exposure");
    exposure.keys["stops"] = std::vector<ParamKey>{
        ParamKey{ 0.0, 0.0, KeyEase::linear() },
        ParamKey{ 1.0, -4.0, KeyEase::linear() },
    };

    const Rational start{ 1, 1 };
    const Rational source_start{ 2, 1 };
    const Rational duration{ 1, 1 };
    const CaseResult result = run_case(gl, media_path, start, source_start, duration, { exposure });

    check(result.eos, "the exposure render reaches EOS");
    check(result.installed, "exposure installs its effect");
    check(result.animated, "a keyed exposure is classified animated");
    if (!result.captured || result.frames.empty()) {
        return;
    }

    // Restrict to frames inside the clip (the sink sees timeline time; frames
    // before the clip start are black and outside the exposure span).
    std::vector<RenderedFrame> in_clip;
    for (const RenderedFrame &frame : result.frames) {
        if (frame.pts_ns >= to_ns(start) && frame.pts_ns < to_ns(start) + to_ns(duration)) {
            in_clip.push_back(frame);
        }
    }
    check(!in_clip.empty(), "frames arrive inside the clip");

    // Three points through the clip: the expected G channel is the green
    // source dimmed by exp2(stops), stops riding 0 -> -4 with the fraction.
    const auto expected_g = [&](std::int64_t pts_ns) {
        const double fraction =
                static_cast<double>(pts_ns - to_ns(start)) / static_cast<double>(to_ns(duration));
        const double stops = -4.0 * std::clamp(fraction, 0.0, 1.0);
        return static_cast<int>(std::llround(255.0 * std::exp2(stops)));
    };

    const int targets[3] = { 10, 50, 90 }; // percent through the clip
    for (const int percent : targets) {
        const std::int64_t at = to_ns(start) + to_ns(duration) * percent / 100;
        const RenderedFrame *frame = frame_nearest(in_clip, at);
        check(frame != nullptr, "a frame sits near the sample point");
        if (frame == nullptr) {
            continue;
        }
        const int expected = expected_g(frame->pts_ns);
        const int diff = std::abs(static_cast<int>(frame->g) - expected);
        check(diff <= 25,
              "exposure follows the clip-local fraction at " + std::to_string(percent) + "% (G "
                      + std::to_string(frame->g) + " vs expected " + std::to_string(expected)
                      + ")");
        std::printf("  [exposure@%d%%] fraction=%.2f G=%u expected=%d\n", percent,
                    static_cast<double>(frame->pts_ns - to_ns(start))
                            / static_cast<double>(to_ns(duration)),
                    frame->g, expected);
    }
}

// An unknown pack followed by a known one: the unknown link is skipped with a
// diagnostic and the known effect still runs.
void test_unknown_plus_known(GlContext &gl, const std::string &media_path)
{
    std::vector<AppliedFilter> effects;
    effects.push_back(AppliedFilter::create("genesis.unknown.pack"));
    effects.push_back(AppliedFilter::create("genesis.invert"));

    const CaseResult result =
            run_case(gl, media_path, Rational{ 0, 1 }, Rational{ 0, 1 }, Rational{ 1, 1 }, effects);

    check(result.eos, "the unknown+known render reaches EOS");
    check(result.installed, "the known effect still installs");
    check(result.top_effect_count == 1, "the unknown effect is omitted (one top effect, not two)");
    check(result.glshader_count == 1, "the known effect's glshader lands");

    bool warned = false;
    for (const std::string &message : result.install_warnings) {
        if (message.find("genesis.unknown.pack") != std::string::npos
            || message.find("unknown pack") != std::string::npos) {
            warned = true;
        }
    }
    check(warned, "the unknown link is reported, not dropped silently");

    if (!result.captured || result.frames.empty()) {
        return;
    }
    const RenderedFrame first = result.frames.front();
    check(first.r > first.g + 100 && first.b > first.g + 100,
          "the known invert still turns green magenta");
    report("unknown+invert", first);
}

// A CPU-only pack (genesis.video-balance: two named-intermediate passes the
// glshader route cannot express, with a `[cpu]` fallback onto the stock
// `videobalance` element) installs one top effect, no glshader, and no probe
// (a CPU-only chain has no shader to probe, so configure returns false and the
// caller frees the chain). The brightness knob lands on the element's own
// double property, darkening the green source.
void test_cpu_fallback_darkens_pixels(GlContext &gl, const std::string &media_path)
{
    AppliedFilter balance = AppliedFilter::create("genesis.video-balance");
    balance.params["brightness"] = -0.8;

    const CaseResult result = run_case(gl, media_path, Rational{ 0, 1 }, Rational{ 0, 1 },
                                       Rational{ 1, 1 }, { std::move(balance) });

    check(result.eos, "the CPU-fallback render reaches EOS");
    check(result.installed, "the CPU fallback installs one top effect");
    check(!result.configured,
          "a CPU-only chain has no shader to probe, so configure returns false");
    check(result.top_effect_count == 1, "one top effect (the videobalance) is on the clip");
    check(result.glshader_count == 0, "no glshader for a CPU-only effect");
    if (!result.captured || result.frames.empty()) {
        return;
    }
    const RenderedFrame first = result.frames.front();
    check(first.g < 150,
          "the brightness fallback darkens the green source (G " + std::to_string(first.g) + ")");
    check(first.g > first.r + 50 && first.g > first.b + 50,
          "the picture stays green-dominant (contrast and saturation unity)");
    report("videobalance(brightness=-0.8)", first);
}

// Injecting an availability probe that refuses every element forces the same
// pack to degrade to the R9 skip: nothing installs, the source stays green,
// and the skip is reported rather than dropped silently.
void test_cpu_fallback_degrades_when_unavailable(GlContext &gl, const std::string &media_path)
{
    const CaseResult result =
            run_case(gl, media_path, Rational{ 0, 1 }, Rational{ 0, 1 }, Rational{ 1, 1 },
                     { AppliedFilter::create("genesis.video-balance") },
                     [](const std::string &) { return false; });

    check(result.eos, "the degraded render still reaches EOS");
    check(!result.installed, "a fallback whose element the probe refuses installs nothing");
    check(result.top_effect_count == 0, "no top effect lands on the clip");
    check(result.glshader_count == 0, "and no glshader either");

    bool warned = false;
    for (const std::string &message : result.install_warnings) {
        if (message.find("genesis.video-balance") != std::string::npos
            || message.find("not expressible") != std::string::npos) {
            warned = true;
        }
    }
    check(warned, "the refused fallback is reported, not dropped silently");

    if (!result.captured || result.frames.empty()) {
        return;
    }
    const RenderedFrame first = result.frames.front();
    check(first.g > first.r + 100 && first.g > first.b + 100,
          "the picture stays green with the effect degraded to a skip");
    report("degraded green", first);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();
    g_log_set_default_handler(on_glib_log, &g_log_warnings);

    test_clip_fraction();
    test_update_clip_uniforms();
    test_update_clip_uniforms_structural_guard();
    test_static_chain_short_circuits();

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
            const MediaFixture media = media_fixture();
            check(!media.path.empty(), "a media fixture is available");
            if (!media.path.empty()) {
                test_baseline_unchanged(gl, media.path);
                test_invert_turns_green_magenta(gl, media.path);
                test_noncommuting_order(gl, media.path);
                test_animated_exposure(gl, media.path);
                test_unknown_plus_known(gl, media.path);
                test_cpu_fallback_darkens_pixels(gl, media.path);
                test_cpu_fallback_degrades_when_unavailable(gl, media.path);
            }
            if (media.owns_file) {
                std::filesystem::remove(media.path);
            }
        }
    }

    return genesis::test::summary();
}
