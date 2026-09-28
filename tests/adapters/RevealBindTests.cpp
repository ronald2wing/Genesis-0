// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gst.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "adapters/engine/ges/session/GesBuilder.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "core/ShaderPass.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

// Headless, no-framework: a self-created EGL context the way the other adapter
// suites do (TextureMixTests.cpp, ShaderChainTests.cpp). This suite drives the
// reveal routing branch - a pass carrying a `reveal_map` builds a texturemix
// bin whose second input is the baked reveal order, not a glshader chain. The
// decisive proof is the same one the custom-element spike settled: the second
// sampler must be independently bound, so a map that reveals the left half and
// hides the right yields green left, transparent black right - a map aliased to
// the picture would leave both halves green.

namespace {

using genesis::adapters::engine::ges::fragment_source_for;
using genesis::adapters::engine::ges::make_reveal_bin;
using genesis::adapters::engine::ges::pass_is_expressible;
using genesis::core::RevealMap;
using genesis::core::ShaderPass;

using genesis::test::check;

// The reveal map's canvas: 64x64, left half reveals first (order 0), right half
// reveals last (order 255). Small so the map is 4096 bytes and the picture
// chain can force the matching size cheaply.
constexpr std::uint32_t kW = 64;
constexpr std::uint32_t kH = 64;
constexpr std::int32_t kSize = 64;
constexpr std::int32_t kHalf = 32;

// The texturemix-convention body: reads the picture (tex0) and the map (tex1),
// and keeps the picture only where the map's order has passed `progress`. It
// declares its own non-sampler uniform - the texturemix owns the prelude and
// the tex0/tex1 samplers, so this body never goes through `fragment_source_for`.
constexpr const char *kRevealBody =
        "uniform float progress;"
        " void main () { vec4 c = texture2D (tex0, v_texcoord);"
        " float order = texture2D (tex1, v_texcoord).r;"
        " gl_FragColor = (order <= progress) ? c : vec4 (0.0, 0.0, 0.0, 0.0); }";

// A pass carrying a reveal map, with `progress` baked into its uniform block so
// `uniforms_for` uploads it (the same field/param layout the resolver produces).
ShaderPass reveal_pass(float progress)
{
    ShaderPass pass;
    pass.package = "test.reveal";
    pass.key = "test.reveal@1#0000000000000000";
    pass.source = std::make_shared<const std::string>(kRevealBody);
    const std::vector<std::array<std::int32_t, 4>> rects = {
        { 0, 0, kHalf, kSize },
        { kHalf, 0, kHalf, kSize },
    };
    pass.reveal_map = RevealMap::from_rects(kW, kH, rects);
    pass.fields = { { "progress", "float", 0, 4 } };
    pass.params = std::vector<std::uint8_t>(ShaderPass::MIN_PARAMS, 0);
    std::memcpy(pass.params.data(), &progress, sizeof(progress));
    return pass;
}

// A pass with no reveal map and a glshader-convention body: the normal path,
// which must be untouched by the reveal branch.
ShaderPass plain_pass()
{
    ShaderPass pass;
    pass.package = "test.plain";
    pass.key = "test.plain@1#0000000000000000";
    pass.source = std::make_shared<const std::string>(
            "void main () { gl_FragColor = texture2D (tex, v_texcoord); }");
    return pass;
}

// How many elements of `factory_name` a bin holds, recursively - the same walk
// EffectApplicationTests uses to count glshaders, here to prove the reveal
// branch is a texturemix and not a glshader chain.
std::size_t count_elements(GstBin *bin, const char *factory_name)
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
                && g_strcmp0(GST_OBJECT_NAME(GST_OBJECT(factory)), factory_name) == 0) {
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

// What the frame-inspection sink records (GL residency), written by the
// streaming thread and read after teardown.
struct ResidencyLog
{
    int frames = 0;
    int gl_memory = 0;
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
        GstCapsFeatures *features = gst_caps_get_features(caps, 0);
        if (gst_caps_features_contains(features, "memory:GLMemory")) {
            ++log->gl_memory;
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

struct RunResult
{
    bool eos = false;
    std::vector<std::uint8_t> bytes;
    bool captured = false;
};

// Drives the reveal bin: a green picture into the ghost sink, the map bytes
// pushed into the internal "map" appsrc, the revealed picture out through the
// ghost src to a residency tee and a capture branch. Returns EOS, residency,
// and the captured RGBA bytes. Takes ownership of `reveal_bin` (it is sunk into
// the pipeline this helper builds and freed with it), so the caller must not
// unref it.
RunResult run_reveal_bin(GlContext &gl, GstElement *reveal_bin,
                         const std::vector<std::uint8_t> &map_bytes, ResidencyLog &residency)
{
    RunResult result;
    GstElement *pipeline = gst_pipeline_new("test");
    if (pipeline == nullptr) {
        // gst_pipeline_new cannot fail in practice; release the floating bin
        // we were handed rather than leak it.
        gst_object_unref(reveal_bin);
        return result;
    }
    gst_bin_add(GST_BIN(pipeline), reveal_bin);

    // Picture: green, forced to the map's canvas, up to RGBA GLMemory - the
    // same head the glshader suites build.
    GstElement *src = gst_element_factory_make("videotestsrc", nullptr);
    GstElement *size_filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *upload = gst_element_factory_make("glupload", nullptr);
    GstElement *colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *gl_filter = gst_element_factory_make("capsfilter", nullptr);
    if (src == nullptr || size_filter == nullptr || convert == nullptr || upload == nullptr
        || colorconvert == nullptr || gl_filter == nullptr) {
        gst_clear_object(&src);
        gst_clear_object(&size_filter);
        gst_clear_object(&convert);
        gst_clear_object(&upload);
        gst_clear_object(&colorconvert);
        gst_clear_object(&gl_filter);
        gst_object_unref(pipeline);
        return result;
    }
    gst_util_set_object_arg(G_OBJECT(src), "pattern", "green");
    g_object_set(src, "num-buffers", 1, nullptr);
    GstCaps *size_caps = gst_caps_new_simple(
            "video/x-raw", "width", G_TYPE_INT, static_cast<gint>(kW), "height", G_TYPE_INT,
            static_cast<gint>(kH), "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
    g_object_set(size_filter, "caps", size_caps, nullptr);
    gst_caps_unref(size_caps);
    GstCaps *gl_caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(gl_filter, "caps", gl_caps, nullptr);
    gst_caps_unref(gl_caps);
    gst_bin_add_many(GST_BIN(pipeline), src, size_filter, convert, upload, colorconvert, gl_filter,
                     nullptr);
    if (!gst_element_link_many(src, size_filter, convert, upload, colorconvert, gl_filter, nullptr)
        || !gst_element_link(gl_filter, reveal_bin)) {
        gst_object_unref(pipeline);
        return result;
    }

    // Two sinks via a tee: one proves GL residency, one captures RGBA bytes.
    GstElement *tee = gst_element_factory_make("tee", nullptr);
    GstElement *q1 = gst_element_factory_make("queue", nullptr);
    GstElement *residency_sink = gst_element_factory_make("appsink", "r");
    GstElement *q2 = gst_element_factory_make("queue", nullptr);
    GstElement *download = gst_element_factory_make("gldownload", nullptr);
    GstElement *dl_convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *capture_sink = gst_element_factory_make("appsink", "c");
    if (tee == nullptr || q1 == nullptr || residency_sink == nullptr || q2 == nullptr
        || download == nullptr || dl_convert == nullptr || capture_sink == nullptr) {
        gst_clear_object(&tee);
        gst_clear_object(&q1);
        gst_clear_object(&residency_sink);
        gst_clear_object(&q2);
        gst_clear_object(&download);
        gst_clear_object(&dl_convert);
        gst_clear_object(&capture_sink);
        gst_object_unref(pipeline);
        return result;
    }
    g_object_set(residency_sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    g_object_set(capture_sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    GstCaps *sink_caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(capture_sink, "caps", sink_caps, nullptr);
    gst_caps_unref(sink_caps);
    gst_bin_add_many(GST_BIN(pipeline), tee, q1, residency_sink, q2, download, dl_convert,
                     capture_sink, nullptr);
    if (!gst_element_link_many(reveal_bin, tee, nullptr)
        || !gst_element_link_many(tee, q1, residency_sink, nullptr)
        || !gst_element_link_many(tee, q2, download, dl_convert, capture_sink, nullptr)) {
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

    GstElement *map_src = gst_bin_get_by_name(GST_BIN(reveal_bin), "map");
    if (map_src == nullptr) {
        gst_object_unref(pipeline);
        return result;
    }

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Push the map as one GRAY8 frame at the picture's first timestamp, then
    // EOS - the aggregator matches the map frame to the picture frame by time.
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, map_bytes.size(), nullptr);
    GstMapInfo info;
    gst_buffer_map(buffer, &info, GST_MAP_WRITE);
    std::memcpy(info.data, map_bytes.data(), map_bytes.size());
    gst_buffer_unmap(buffer, &info);
    GST_BUFFER_PTS(buffer) = 0;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
    gst_app_src_push_buffer(GST_APP_SRC(map_src), buffer);
    gst_app_src_end_of_stream(GST_APP_SRC(map_src));
    gst_object_unref(map_src);

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

// True when the pixel at (x, y) of a width*height RGBA buffer is within
// `tolerance` of (r, g, b).
bool pixel_near(const std::vector<std::uint8_t> &bytes, std::uint32_t width, std::uint32_t x,
                std::uint32_t y, int r, int g, int b, int tolerance)
{
    const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
    if (index + 3 >= bytes.size()) {
        return false;
    }
    return std::abs(static_cast<int>(bytes[index]) - r) <= tolerance
            && std::abs(static_cast<int>(bytes[index + 1]) - g) <= tolerance
            && std::abs(static_cast<int>(bytes[index + 2]) - b) <= tolerance;
}

void test_reveal_pass_routes_to_a_texturemix_bin()
{
    GstElement *bin = make_reveal_bin(reveal_pass(0.5f));
    check(bin != nullptr, "a reveal pass builds a bin");
    if (bin == nullptr) {
        return;
    }
    check(GST_IS_BIN(bin), "the reveal result is a GstBin");
    check(count_elements(GST_BIN(bin), "texturemix") == 1,
          "the reveal bin holds exactly one texturemix");
    check(count_elements(GST_BIN(bin), "glshader") == 0, "the reveal bin holds no glshader");
    gst_object_unref(bin);
}

void test_plain_pass_stays_on_the_glshader_path()
{
    const ShaderPass plain = plain_pass();

    GstElement *bin = make_reveal_bin(plain);
    check(bin == nullptr, "a pass with no reveal map builds no bin");

    // The normal path is untouched: the plain pass is still expressible as one
    // glshader, and its fragment source still follows the single-`tex`
    // convention.
    check(pass_is_expressible(plain), "the plain pass is still glshader-expressible");
    const std::optional<std::string> source = fragment_source_for(plain);
    check(source.has_value(), "the plain pass still emits a fragment source");
    check(source.has_value() && source->find("uniform sampler2D tex") != std::string::npos,
          "the plain pass still carries the single-texture prelude");
}

void test_reveal_bin_applies_the_map(GlContext &gl)
{
    const ShaderPass pass = reveal_pass(0.5f);
    GstElement *bin = make_reveal_bin(pass);
    check(bin != nullptr, "the reveal pass builds a bin for the drive test");
    if (bin == nullptr) {
        return;
    }

    ResidencyLog residency;
    RunResult result = run_reveal_bin(gl, bin, *pass.reveal_map->gray, residency);

    check(result.eos, "the reveal pipeline reaches EOS");
    check(result.captured, "the revealed output captures");
    check(residency.frames > 0, "at least one frame arrives");
    check(residency.gl_memory == residency.frames, "every frame is GLMemory");
    if (!result.captured || result.bytes.empty()) {
        return;
    }

    // The decisive proof, the spike's anti-aliasing check: progress 0.5 with
    // the left half at order 0 and the right half at order 255 must keep the
    // left green and hide the right. If tex1 silently aliased tex0 (or the
    // map never reached the shader), both halves would stay green.
    check(pixel_near(result.bytes, kW, kW / 4, kH / 2, 0, 255, 0, 12),
          "the revealed left half is the green picture");
    check(pixel_near(result.bytes, kW, 3 * kW / 4, kH / 2, 0, 0, 0, 12),
          "the hidden right half is black (the map ordered it hidden)");
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

    test_reveal_bin_applies_the_map(gl);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);

    // No GL needed: the bin structure and the routing/regression refusals.
    test_reveal_pass_routes_to_a_texturemix_bin();
    test_plain_pass_stays_on_the_glshader_path();

    // GL-backed: drive the bin and prove the map is independently bound.
    run_gl_tests();

    return genesis::test::summary();
}
