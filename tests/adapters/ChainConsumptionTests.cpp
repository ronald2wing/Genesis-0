// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <ges/ges.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>

#include "adapters/engine/ges/session/GesBuilder.h"
#include "project/command/ClipCommands.h"
#include "project/command/Command.h"
#include "project/command/MediaCommands.h"
#include "project/command/PropertyCommands.h"
#include "project/model/Effects.h"
#include "project/model/Keyframe.h"
#include "project/model/Media.h"
#include "project/model/Text.h"
#include "project/Editor.h"
#include "render/Flatten.h"
#include "render/Resolve.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

// Headless, no-framework: proves an ordinary clip's effect chain renders
// through the real production path - Editor commands -> flatten ->
// render::build_timeline -> ges::build_timeline - rather than a hand-built
// graph. The built GES timeline is driven through a GESPipeline in preview
// mode and the RGBA output of a custom video sink is captured, exactly like
// ClipEffectTests.cpp. `built.chains` (populated by resolve) is what the
// builder consumes, so a green fixture turning magenta under genesis.invert is
// the end-to-end evidence the chain reached the production graph.

namespace {

using genesis::test::check;

using namespace genesis::project;
namespace render = genesis::render;
namespace ges_adapter = genesis::adapters::engine::ges;
using genesis::core::Rational;

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
            std::filesystem::temp_directory_path() / "genesis-chain-consumption.webm";
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

// One captured frame: the timeline-time PTS and three RGB samples at
// mid-height - the left, middle and right thirds. The middle sample is what
// the original tests read; the left and right samples are what the reveal test
// reads to prove the mask varies across the canvas, not only at its centre.
struct RenderedFrame
{
    std::int64_t pts_ns = 0;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t lr = 0;
    std::uint8_t lg = 0;
    std::uint8_t lb = 0;
    std::uint8_t rr = 0;
    std::uint8_t rg = 0;
    std::uint8_t rb = 0;
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
            const auto pixel = [&](std::size_t x, std::size_t y) {
                return (static_cast<std::size_t>(y) * width + x) * 4;
            };
            const std::size_t mid_y = static_cast<std::size_t>(height) / 2;
            const std::size_t index = pixel(static_cast<std::size_t>(width) / 2, mid_y);
            const std::size_t left = pixel(static_cast<std::size_t>(width) / 6, mid_y);
            const std::size_t right = pixel(static_cast<std::size_t>(width) * 5 / 6, mid_y);
            if (right + 3 < info.size) {
                RenderedFrame frame;
                frame.pts_ns = GST_BUFFER_PTS_IS_VALID(buffer)
                        ? static_cast<std::int64_t>(GST_BUFFER_PTS(buffer))
                        : 0;
                frame.r = info.data[index];
                frame.g = info.data[index + 1];
                frame.b = info.data[index + 2];
                frame.lr = info.data[left];
                frame.lg = info.data[left + 1];
                frame.lb = info.data[left + 2];
                frame.rr = info.data[right];
                frame.rg = info.data[right + 1];
                frame.rb = info.data[right + 2];
                log->frames.push_back(frame);
            }
            gst_buffer_unmap(buffer, &info);
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// The outcome of one production-path render: whether it reached EOS, the
// captured frames, and the structural facts gathered from the built timeline.
struct RenderResult
{
    bool eos = false;
    bool captured = false;
    std::string error;
    std::vector<RenderedFrame> frames;
    std::size_t chain_count = 0;
    std::size_t treatment_count = 0;
    std::size_t glshader_count = 0;
};

// Drives the whole production path: flatten the editor's project, resolve the
// built timeline, build the GES graph, render it through a GESPipeline preview,
// and capture the RGBA samples of every frame. `title_images` and `reveal_maps`
// are the same maps app::build_for_engine hands `render::build_timeline` - a
// title clip's rasterised picture and its per-word reveal map, keyed by clip
// id - so a reveal case drives the exact seam the app does.
RenderResult
render_project(GlContext &gl, Editor &editor,
               const std::unordered_map<std::string, std::string> &title_images = { },
               const std::unordered_map<std::string, genesis::core::RevealMap> &reveal_maps = { })
{
    RenderResult result;

    const std::vector<render::ExportClip> flat =
            render::flatten_timeline(editor.project(), std::nullopt);
    render::ExportRequest request;
    request.width = 320;
    request.height = 180;
    const render::BuiltTimeline built = render::build_timeline(
            request, genesis::core::FrameRate::THIRTY, std::span<const render::ExportClip>(flat),
            { }, title_images, reveal_maps);
    result.chain_count = built.chains.size();
    result.treatment_count = built.treatments.size();

    GESTimeline *timeline = ges_adapter::build_timeline(built);
    if (timeline == nullptr) {
        result.error = "the GES timeline did not build";
        return result;
    }
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

// Adds the green fixture as media and cuts one clip of it at timeline 0. The
// four-second media duration is what the head-trim in the exposure case later
// shortens.
struct Fixture
{
    Editor editor;
    std::string clip_id;
};

Fixture add_clip(const std::string &media_path)
{
    Fixture f;
    NewMedia item;
    item.path = media_path;
    item.name = "fixture.webm";
    item.duration = Rational{ 4, 1 };
    item.kind = MediaKind::Video;
    item.width = 320;
    item.height = 180;
    item.has_audio = false;
    const auto add_media = f.editor.apply(Command{ AddMedia{ std::move(item) } });
    check(add_media.has_value() && add_media->created_id.has_value(), "adds media");
    const std::string media_id = *add_media->created_id;
    const std::string track_id = f.editor.project().timelines[0].tracks[0].id;
    const auto add_clip =
            f.editor.apply(Command{ AddClip{ media_id, track_id, Rational{ 0, 1 }, false } });
    check(add_clip.has_value() && add_clip->created_id.has_value(), "adds a clip");
    f.clip_id = *add_clip->created_id;
    return f;
}

void set_effects(Editor &editor, const std::string &clip_id, std::vector<AppliedFilter> effects)
{
    ClipPatch patch;
    patch.video_effects = std::move(effects);
    check(editor.apply(Command{ UpdateClip{ clip_id, std::move(patch) } }).has_value(),
          "sets the video effects");
}

void test_baseline_unchanged(GlContext &gl, const std::string &media_path)
{
    Fixture f = add_clip(media_path);

    const RenderResult result = render_project(gl, f.editor);

    check(result.eos, "the baseline render reaches EOS");
    check(result.chain_count == 0, "no effects resolve no clip chain");
    check(result.glshader_count == 0, "no effects add no glshader");
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
    Fixture f = add_clip(media_path);
    set_effects(f.editor, f.clip_id, { AppliedFilter::create("genesis.invert") });

    const RenderResult result = render_project(gl, f.editor);

    check(result.eos, "the invert render reaches EOS");
    check(result.chain_count == 1, "one clip carries one chain");
    check(result.glshader_count == 1, "one glshader for the one-pass invert");
    if (!result.captured || result.frames.empty()) {
        return;
    }
    const RenderedFrame first = result.frames.front();
    check(first.r > first.g + 100 && first.b > first.g + 100,
          "invert turns green magenta (R and B high, G low)");
    report("invert(green)", first);
}

// Two noncommuting passes through the production path: invert then sepia gives
// a warm brown, sepia then invert a cool blue. The order in which the two top
// effects run is therefore observable in the pixels, proving the chain order
// from `built.chains` survives the builder.
void test_noncommuting_order(GlContext &gl, const std::string &media_path)
{
    Fixture a = add_clip(media_path);
    set_effects(
            a.editor, a.clip_id,
            { AppliedFilter::create("genesis.invert"), AppliedFilter::create("genesis.sepia") });
    const RenderResult invert_first_result = render_project(gl, a.editor);

    Fixture b = add_clip(media_path);
    set_effects(
            b.editor, b.clip_id,
            { AppliedFilter::create("genesis.sepia"), AppliedFilter::create("genesis.invert") });
    const RenderResult sepia_first_result = render_project(gl, b.editor);

    check(invert_first_result.eos && sepia_first_result.eos, "both two-pass renders reach EOS");
    check(invert_first_result.chain_count == 1 && sepia_first_result.chain_count == 1,
          "each clip carries its chain");
    check(invert_first_result.glshader_count == 2 && sepia_first_result.glshader_count == 2,
          "two glshaders for the two passes");
    if (!invert_first_result.captured || invert_first_result.frames.empty()
        || !sepia_first_result.captured || sepia_first_result.frames.empty()) {
        return;
    }
    const RenderedFrame warm = invert_first_result.frames.front();
    const RenderedFrame cool = sepia_first_result.frames.front();
    report("invert-then-sepia", warm);
    report("sepia-then-invert", cool);

    // invert then sepia: green -> magenta -> sepia(magenta) = warm, R > G > B.
    check(warm.r > warm.g && warm.g > warm.b, "invert-then-sepia is warm (R > G > B)");
    // sepia then invert: green -> sepia(green) -> invert = cool, B > G > R.
    check(cool.b > cool.g && cool.g > cool.r, "sepia-then-invert is cool (B > G > R)");
    check(warm.r > warm.b && cool.b > cool.r, "the two orders produce different pictures");
}

// Animated exposure through the production path, with a NONZERO source inpoint
// reached by a head trim: the clip is trimmed two seconds off its head, so it
// starts at 2s on the timeline and reads from 2s in the source, running two
// seconds. The clip-local fraction the probe feeds the keyed knob is only
// right if the source inpoint is subtracted - a wrong absolute-time fraction
// clamps to 0 or 1 and the brightness stops moving.
void test_animated_exposure(GlContext &gl, const std::string &media_path)
{
    Fixture f = add_clip(media_path);

    // Trim the head two seconds: start 0->2, source_start 0->2, duration 4->2.
    const auto trim = f.editor.apply(
            Command{ TrimClip{ f.clip_id, TrimEdge::Start, Rational{ 2, 1 }, false } });
    check(trim.has_value(), "trims the head to a two-second inpoint");

    AppliedFilter exposure = AppliedFilter::create("genesis.exposure");
    exposure.keys["stops"] = std::vector<ParamKey>{
        ParamKey{ 0.0, 0.0, KeyEase::linear() },
        ParamKey{ 1.0, -4.0, KeyEase::linear() },
    };
    set_effects(f.editor, f.clip_id, { std::move(exposure) });

    const RenderResult result = render_project(gl, f.editor);

    check(result.eos, "the exposure render reaches EOS");
    check(result.chain_count == 1, "the clip carries one chain");
    if (!result.captured || result.frames.empty()) {
        return;
    }

    const Rational start{ 2, 1 };
    const Rational duration{ 2, 1 };

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

// A solid red PNG, 320x180: the title picture the reveal test rasterises. Red
// is the load-bearing choice - a revealed pixel is red, a hidden one composites
// to black - so the mask is a channel-dominance comparison robust to the PNG's
// decode. Returns an empty path on failure.
std::string red_png_fixture()
{
    const std::filesystem::path generated =
            std::filesystem::temp_directory_path() / "genesis-chain-title.png";
    std::filesystem::remove(generated);
    const std::string command = "gst-launch-1.0 -q videotestsrc pattern=red num-buffers=1 ! "
                                "video/x-raw,width=320,height=180,framerate=1/1 ! videoconvert ! "
                                "pngenc ! filesink location=\""
            + generated.string() + "\" >/dev/null 2>&1";
    if (std::system(command.c_str()) == 0 && std::filesystem::exists(generated)
        && std::filesystem::file_size(generated) > 0) {
        return generated.string();
    }
    std::filesystem::remove(generated);
    return "";
}

// The per-word reveal map the app bakes beside the title picture: three
// vertical thirds ordered left (0), middle (128), right (255) - the same mask
// table RevealEffectTests drives the reusable effect with. The middle byte
// normalises to 0.50196, so the body's `order <= progress` keeps the middle
// hidden at 0.5 and reveals it at 0.75.
genesis::core::RevealMap three_word_map(std::uint32_t width, std::uint32_t height)
{
    const std::int32_t third = static_cast<std::int32_t>(width / 3);
    const std::vector<std::array<std::int32_t, 4>> rects = {
        { 0, 0, third, static_cast<std::int32_t>(height) },
        { third, 0, static_cast<std::int32_t>(width) - 2 * third,
          static_cast<std::int32_t>(height) },
        { 2 * third, 0, static_cast<std::int32_t>(width) - 2 * third,
          static_cast<std::int32_t>(height) },
    };
    return genesis::core::RevealMap::from_rects(width, height, rects);
}

// One region's sample: revealed pixels carry the red title, hidden pixels the
// transparent black that composites over black. The two are far apart, so the
// channel comparison is decisive.
void check_region(std::uint8_t r, std::uint8_t g, std::uint8_t b, bool revealed,
                  const std::string &what)
{
    if (revealed) {
        check(r > 150 && g < 90 && b < 90,
              what + "revealed red (rgb " + std::to_string(r) + "," + std::to_string(g) + ","
                      + std::to_string(b) + ")");
    } else {
        check(r < 90 && g < 90 && b < 90,
              what + "hidden black (rgb " + std::to_string(r) + "," + std::to_string(g) + ","
                      + std::to_string(b) + ")");
    }
}

// The end-to-end reveal path: a title clip carrying a `genesis.reveal` filter,
// driven through Editor commands -> flatten -> render::build_timeline ->
// ges::build_timeline -> GESPipeline capture, with the title picture and the
// per-word reveal map supplied beside it the way app::build_for_engine
// supplies them. The reveal must mask the picture: at progress 0 only the left
// word shows, and left/middle/right come in monotonically as progress rises. A
// reveal that fails to mask must FAIL.
void test_reveal_masks_the_title(GlContext &gl)
{
    Editor editor;
    const std::string track_id = editor.project().timelines[0].tracks[0].id;
    const auto added = editor.apply(
            Command{ AddTextClip{ std::optional<std::string>{ track_id }, false, Rational{ 0, 1 },
                                  std::optional<TextStyle>{ },
                                  std::optional<Rational>{ Rational{ 1, 1 } }, std::nullopt } });
    check(added.has_value() && added->created_id.has_value(), "adds a title");
    if (!added.has_value() || !added->created_id.has_value()) {
        return;
    }
    const std::string clip_id = *added->created_id;

    const std::string png = red_png_fixture();
    check(!png.empty(), "the title PNG fixture is available");
    if (png.empty()) {
        return;
    }
    std::unordered_map<std::string, std::string> images;
    images.emplace(clip_id, png);
    std::unordered_map<std::string, genesis::core::RevealMap> maps;
    maps.emplace(clip_id, three_word_map(320, 180));

    // The mask table: as progress rises 0 -> 0.5 -> 0.75 -> 1, the three
    // thirds reveal left, then middle, then right - monotonic, never
    // un-revealing.
    const struct
    {
        double progress;
        bool left;
        bool middle;
        bool right;
    } cases[] = {
        { 0.0, true, false, false },
        { 0.5, true, false, false },
        { 0.75, true, true, false },
        { 1.0, true, true, true },
    };

    for (const auto &c : cases) {
        AppliedFilter reveal = AppliedFilter::create("genesis.reveal");
        reveal.params["progress"] = c.progress;
        set_effects(editor, clip_id, { std::move(reveal) });

        const RenderResult result = render_project(gl, editor, images, maps);

        const std::string prefix = "progress " + std::to_string(c.progress) + ": ";
        check(result.eos, prefix + "the reveal render reaches EOS");
        check(result.chain_count == 1,
              prefix + "the title's reveal filter reaches the built chain");
        if (!result.captured || result.frames.empty()) {
            continue;
        }
        const RenderedFrame first = result.frames.front();
        check_region(first.lr, first.lg, first.lb, c.left, prefix + "left ");
        check_region(first.r, first.g, first.b, c.middle, prefix + "middle ");
        check_region(first.rr, first.rg, first.rb, c.right, prefix + "right ");
    }

    std::filesystem::remove(png);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    ges_init();

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
                test_reveal_masks_the_title(gl);
            }
            if (media.owns_file) {
                std::filesystem::remove(media.path);
            }
        }
    }

    return genesis::test::summary();
}
