// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <gst/app/gstappsink.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gst.h>
#include <optional>
#include <string>
#include <vector>

#include "adapters/engine/ges/effects/TextureMix.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

// Headless, no-framework: a self-created EGL context the way the other adapter
// suites do (FrameTextureTests.cpp, ShaderChainTests.cpp). The element is a
// GstGLMixer subclass (the spike's gltexmix, brought into the adapter): it
// binds N input textures into one fragment shader, each on its own unit with
// an explicit glUniform1i, so a second sampler2D can never silently alias unit
// 0 (docs/decisions/effect-execution.md §6).

namespace {

using genesis::adapters::engine::ges::make_texture_mix;
using genesis::adapters::engine::ges::register_texture_mix;

using genesis::test::check;

// The headless EGL display + context the GL elements borrow. Answers the
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

// The two-input fragment bodies. The element owns the prelude (precision, the
// varying, and the tex0..tex7 samplers), so each body only names the samplers
// it samples and declares its own non-sampler uniforms.
//
// combine: R from tex0, G from tex1. With tex0 = solid red and tex1 = solid
// green this yields YELLOW only when the two samplers are bound to distinct
// textures; if tex1 silently aliased tex0 the output would be RED.
constexpr const char *kCombineBody = "void main () { vec4 a = texture2D (tex0, v_texcoord);"
                                     " vec4 b = texture2D (tex1, v_texcoord);"
                                     " gl_FragColor = vec4 (a.r, b.g, 0.0, 1.0); }";

// tex1-only: sample ONLY tex1 and ignore tex0 - the anti-alias control. With
// tex0 = red and tex1 = green the output is GREEN if tex1 is independently
// bound, RED if it aliases tex0.
constexpr const char *kTex1Body = "void main () { gl_FragColor = texture2D (tex1, v_texcoord); }";

// blend: mix tex0 into tex1 by the `amount` uniform. amount = 0 gives tex0,
// amount = 1 gives tex1; the uniform upload must move the output.
constexpr const char *kBlendBody = "uniform float amount;"
                                   " void main () { vec4 a = texture2D (tex0, v_texcoord);"
                                   " vec4 b = texture2D (tex1, v_texcoord);"
                                   " gl_FragColor = mix (a, b, amount); }";

// A source chain that emits `pattern` as RGBA GLMemory, the capsfilter the
// spike proved load-bearing. Returns the capsfilter tail to link from, or
// nullptr on failure. Added to the pipeline.
GstElement *add_solid_source(GstBin *pipeline, const char *pattern, int num_buffers)
{
    GstElement *src = gst_element_factory_make("videotestsrc", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *upload = gst_element_factory_make("glupload", nullptr);
    GstElement *colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    if (src == nullptr || convert == nullptr || upload == nullptr || colorconvert == nullptr
        || filter == nullptr) {
        gst_clear_object(&src);
        gst_clear_object(&convert);
        gst_clear_object(&upload);
        gst_clear_object(&colorconvert);
        gst_clear_object(&filter);
        return nullptr;
    }
    gst_util_set_object_arg(G_OBJECT(src), "pattern", pattern);
    g_object_set(src, "num-buffers", num_buffers, nullptr);
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    gst_bin_add_many(pipeline, src, convert, upload, colorconvert, filter, nullptr);
    if (!gst_element_link_many(src, convert, upload, colorconvert, filter, nullptr)) {
        // The pipeline already owns the added elements; the caller unreffing
        // it reclaims them.
        return nullptr;
    }
    return filter;
}

// Links `source` (a chain tail) to a fresh requested sink pad on `mix`.
bool link_to_sink(GstElement *mix, GstElement *source)
{
    GstPad *sink_pad = gst_element_request_pad_simple(mix, "sink_%u");
    if (sink_pad == nullptr) {
        return false;
    }
    GstPad *src_pad = gst_element_get_static_pad(source, "src");
    const bool linked = src_pad != nullptr && gst_pad_link(src_pad, sink_pad) == GST_PAD_LINK_OK;
    if (src_pad != nullptr) {
        gst_object_unref(src_pad);
    }
    gst_object_unref(sink_pad);
    return linked;
}

// What the frame-inspection sink records (GL residency), written by the
// streaming thread and read after teardown.
struct ResidencyLog
{
    int frames = 0;
    int gl_memory = 0;
    bool texture_2d = false;
};

GstFlowReturn on_residency_sample(GstAppSink *sink, gpointer user_data)
{
    auto *log = static_cast<ResidencyLog *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    ++log->frames;
    GstCaps *caps = gst_sample_get_caps(sample);
    if (caps != nullptr) {
        gchar *caps_str = gst_caps_to_string(caps);
        g_free(caps_str);
        GstCapsFeatures *features = gst_caps_get_features(caps, 0);
        if (gst_caps_features_contains(features, "memory:GLMemory")) {
            ++log->gl_memory;
        }
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        const char *target = gst_structure_get_string(structure, "texture-target");
        if (target != nullptr && g_strcmp0(target, "2D") == 0) {
            log->texture_2d = true;
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// The downloaded RGBA bytes, captured once for pixel assertions.
struct CaptureLog
{
    std::vector<std::uint8_t> bytes;
    bool captured = false;
};

GstFlowReturn on_capture_sample(GstAppSink *sink, gpointer user_data)
{
    auto *log = static_cast<CaptureLog *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (buffer != nullptr) {
        GstMapInfo info;
        if (gst_buffer_map(buffer, &info, GST_MAP_READ)) {
            log->bytes.assign(info.data, info.data + info.size);
            gst_buffer_unmap(buffer, &info);
            log->captured = true;
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// The outcome of driving a pipeline: whether it reached EOS, and the captured
// bytes (when the capture branch was requested).
struct RunResult
{
    bool eos = false;
    std::vector<std::uint8_t> bytes;
    bool captured = false;
};

// Builds a pipeline with `sources` inputs feeding the texturemix (created with
// `fragment` and `uniforms`), a residency tee, and a capture tee. Runs it to
// EOS and returns the outcome plus the residency log. `sources` may be 0 or 1
// (a single solid source) to exercise the refuse path.
RunResult run_mixer(GlContext &gl, const std::vector<const char *> &sources, const char *fragment,
                    GstStructure *uniforms, ResidencyLog &residency)
{
    RunResult result;
    GstElement *pipeline = gst_pipeline_new("test");
    GstElement *mix = make_texture_mix(fragment, uniforms);
    if (pipeline == nullptr || mix == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&mix);
        return result;
    }
    gst_bin_add(GST_BIN(pipeline), mix);

    // Feed each source into its own requested sink pad.
    for (const char *pattern : sources) {
        GstElement *source = add_solid_source(GST_BIN(pipeline), pattern, 1);
        if (source == nullptr || !link_to_sink(mix, source)) {
            gst_object_unref(pipeline);
            return result;
        }
    }

    // Two sinks via a tee: one proves GL residency, one captures RGBA bytes.
    GstElement *tee = gst_element_factory_make("tee", nullptr);
    GstElement *q1 = gst_element_factory_make("queue", nullptr);
    GstElement *residency_sink = gst_element_factory_make("appsink", "r");
    GstElement *q2 = gst_element_factory_make("queue", nullptr);
    GstElement *download = gst_element_factory_make("gldownload", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *capture_sink = gst_element_factory_make("appsink", "c");
    if (tee == nullptr || q1 == nullptr || residency_sink == nullptr || q2 == nullptr
        || download == nullptr || convert == nullptr || capture_sink == nullptr) {
        gst_object_unref(pipeline);
        return result;
    }
    g_object_set(residency_sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    g_object_set(capture_sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    GstCaps *sink_caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(capture_sink, "caps", sink_caps, nullptr);
    gst_caps_unref(sink_caps);

    gst_bin_add_many(GST_BIN(pipeline), tee, q1, residency_sink, q2, download, convert,
                     capture_sink, nullptr);
    if (!gst_element_link_many(mix, tee, nullptr)
        || !gst_element_link_many(tee, q1, residency_sink, nullptr)
        || !gst_element_link_many(tee, q2, download, convert, capture_sink, nullptr)) {
        gst_object_unref(pipeline);
        return result;
    }

    GstAppSinkCallbacks residency_cb = { };
    residency_cb.new_sample = on_residency_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(residency_sink), &residency_cb, &residency, nullptr);
    CaptureLog capture;
    GstAppSinkCallbacks capture_cb = { };
    capture_cb.new_sample = on_capture_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(capture_sink), &capture_cb, &capture, nullptr);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (message != nullptr) {
        result.eos = GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        gst_message_unref(message);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);

    result.bytes = std::move(capture.bytes);
    result.captured = capture.captured;
    return result;
}

// True when the first pixel is within `tolerance` of (r, g, b).
bool first_pixel_near(const std::vector<std::uint8_t> &bytes, int r, int g, int b, int tolerance)
{
    if (bytes.size() < 4) {
        return false;
    }
    return std::abs(static_cast<int>(bytes[0]) - r) <= tolerance
            && std::abs(static_cast<int>(bytes[1]) - g) <= tolerance
            && std::abs(static_cast<int>(bytes[2]) - b) <= tolerance;
}

void test_registers_and_instantiates()
{
    check(register_texture_mix(), "the element registers");

    GstElement *from_factory = gst_element_factory_make("texturemix", nullptr);
    check(from_factory != nullptr, "the factory makes the registered element");
    if (from_factory != nullptr) {
        gst_object_unref(from_factory);
    }

    GstElement *element = make_texture_mix(kCombineBody, nullptr);
    check(element != nullptr, "make_texture_mix creates an element");
    if (element == nullptr) {
        return;
    }
    check(GST_IS_GL_MIXER(element), "the element is a GstGLMixer");
    gchar *fragment = nullptr;
    g_object_get(element, "fragment", &fragment, nullptr);
    check(fragment != nullptr && g_strcmp0(fragment, kCombineBody) == 0,
          "the fragment body is stored verbatim");
    g_free(fragment);
    gst_object_unref(element);
}

void test_two_input_combine(GlContext &gl)
{
    ResidencyLog residency;
    RunResult result = run_mixer(gl, { "red", "green" }, kCombineBody, nullptr, residency);

    check(result.eos, "the two-input pipeline reaches EOS");
    check(result.captured, "the two-input output captures");
    check(residency.frames > 0, "at least one frame arrives");
    check(residency.gl_memory == residency.frames, "every frame is GLMemory");
    check(residency.texture_2d, "the frames carry texture-target=2D");
    if (!result.captured || result.bytes.empty()) {
        return;
    }

    // The spike's decisive proof: red + green sampled independently yields
    // yellow. An output equal to either input alone is a failure, not a pass.
    const bool yellow = first_pixel_near(result.bytes, 255, 255, 0, 12);
    check(yellow, "the output is yellow (depends on both inputs)");
    check(!first_pixel_near(result.bytes, 255, 0, 0, 12), "the output is not tex0 alone (red)");
    check(!first_pixel_near(result.bytes, 0, 255, 0, 12), "the output is not tex1 alone (green)");
}

void test_second_input_is_independently_bound(GlContext &gl)
{
    ResidencyLog residency;
    RunResult result = run_mixer(gl, { "red", "green" }, kTex1Body, nullptr, residency);

    check(result.eos, "the tex1-only pipeline reaches EOS");
    if (!result.captured || result.bytes.empty()) {
        return;
    }
    // Sampling only tex1 returns green: tex1 is bound to a distinct texture,
    // not aliased to tex0 (which would return red).
    check(first_pixel_near(result.bytes, 0, 255, 0, 12),
          "tex1 samples the second input, not an aliased first");
}

void test_uniforms_are_applied(GlContext &gl)
{
    // amount = 0 returns tex0 (red), amount = 1 returns tex1 (green); the
    // uniform upload must move the output.
    GstStructure *zero = gst_structure_new("uniforms", "amount", G_TYPE_FLOAT, 0.0f, nullptr);
    ResidencyLog residency_zero;
    RunResult at_zero = run_mixer(gl, { "red", "green" }, kBlendBody, zero, residency_zero);
    gst_structure_free(zero);

    GstStructure *one = gst_structure_new("uniforms", "amount", G_TYPE_FLOAT, 1.0f, nullptr);
    ResidencyLog residency_one;
    RunResult at_one = run_mixer(gl, { "red", "green" }, kBlendBody, one, residency_one);
    gst_structure_free(one);

    check(at_zero.eos && at_one.eos, "the uniform pipeline reaches EOS");
    if (!at_zero.captured || at_zero.bytes.empty() || !at_one.captured || at_one.bytes.empty()) {
        return;
    }
    check(first_pixel_near(at_zero.bytes, 255, 0, 0, 12), "amount = 0 returns tex0 (red)");
    check(first_pixel_near(at_one.bytes, 0, 255, 0, 12), "amount = 1 returns tex1 (green)");
    check(!first_pixel_near(at_zero.bytes, 0, 255, 0, 12), "the uniform changed the output");
}

void test_missing_second_input_is_refused(GlContext &gl)
{
    ResidencyLog residency;
    RunResult result = run_mixer(gl, { "red" }, kCombineBody, nullptr, residency);

    // A single input has nothing to combine: the element refuses (an error
    // message, no EOS) rather than silently rendering a partial result.
    check(!result.eos, "a single input is refused, not silently rendered");
}

void run_gl_tests()
{
    GlContext gl;
    gl.display = genesis::test::make_egl_display();
    check(gl.display != nullptr, "a headless EGL display is available");
    if (gl.display == nullptr) {
        return;
    }
    gl.context = gst_gl_context_new(gl.display);
    GError *error = nullptr;
    if (!gst_gl_context_create(gl.context, nullptr, &error)) {
        check(false,
              std::string("a headless GL context creates: ")
                      + (error != nullptr ? error->message : "unknown"));
        g_clear_error(&error);
        return;
    }

    test_two_input_combine(gl);
    test_second_input_is_independently_bound(gl);
    test_uniforms_are_applied(gl);
    test_missing_second_input_is_refused(gl);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    test_registers_and_instantiates();
    run_gl_tests();

    return genesis::test::summary();
}
