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

#include "adapters/engine/ges/effects/LutGrade.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

// Headless, no-framework, like TextureMixTests.cpp: a self-created EGL context
// (GLES2 + llvmpipe). The lutgrade element samples one 2D picture texture and
// one 3D LUT in a single fragment shader, so a colour-look pass with a resolved
// table renders the real table instead of the pack's GLSL approximation.

namespace {

using genesis::adapters::engine::ges::fragment_source_lut_for;
using genesis::adapters::engine::ges::make_lut_grade;
using genesis::adapters::engine::ges::register_lut_grade;
using genesis::adapters::engine::ges::uniforms_for;
using genesis::core::Lut;
using genesis::core::ShaderPass;

using genesis::test::check;

// A 2x2x2 all-green table: every input colour maps to green. Sampling it at
// any colour returns green, which makes the LUT's effect unambiguous.
Lut all_green_lut()
{
    std::vector<float> rgb;
    rgb.reserve(8 * 3);
    for (int i = 0; i < 8; ++i) {
        rgb.push_back(0.0f);
        rgb.push_back(1.0f);
        rgb.push_back(0.0f);
    }
    return *Lut::from_rgb(2, rgb);
}

// A pass shaped like genesis.lut-grade's resolved pass: a `lut` enum field at
// offset 0 and a `strength` field at offset 4, both floats, and the resolved
// table. `strength` is the knob the LUT body mixes by.
ShaderPass make_lut_pass(float strength)
{
    ShaderPass pass;
    pass.package = "genesis.lut-grade";
    pass.key = "test";
    pass.lut = all_green_lut();
    pass.fields = {
        { "lut", "float", 0, 4 },
        { "strength", "float", 4, 4 },
    };
    pass.params = std::vector<std::uint8_t>(ShaderPass::MIN_PARAMS, 0);
    std::memcpy(pass.params.data() + 4, &strength, sizeof(strength));
    return pass;
}

void test_lut_source_emits_the_3d_prelude()
{
    const ShaderPass pass = make_lut_pass(100.0f);
    const std::optional<std::string> source = fragment_source_lut_for(pass);
    check(source.has_value(), "a strength-carrying LUT pass yields a source");
    if (!source.has_value()) {
        return;
    }
    check(source->find("#extension GL_OES_texture_3D") != std::string::npos,
          "the prelude enables the 3D texture extension");
    check(source->find("uniform sampler3D lut3d;") != std::string::npos,
          "the prelude declares the 3D LUT sampler");
    check(source->find("precision mediump sampler3D;") != std::string::npos,
          "the prelude declares the sampler3D precision");
    check(source->find("texture3D (lut3d, c.rgb)") != std::string::npos,
          "the body samples the table at the picture's colour");
    check(source->find("strength / 100.0") != std::string::npos,
          "the body mixes by the declared strength knob");
}

void test_lut_source_refuses_without_strength()
{
    ShaderPass pass;
    pass.package = "genesis.lut-grade";
    pass.lut = all_green_lut();
    pass.fields = { { "lut", "float", 0, 4 } };
    pass.params = std::vector<std::uint8_t>(ShaderPass::MIN_PARAMS, 0);
    check(!fragment_source_lut_for(pass).has_value(),
          "a LUT pass without a strength field is refused");
}

void test_lut_source_refuses_pre_stages()
{
    ShaderPass pass = make_lut_pass(100.0f);
    pass.stages.push_back({ "stage0", { 1, 1 } });
    check(!fragment_source_lut_for(pass).has_value(), "a LUT pass carrying pre-stages is refused");
}

void test_registers_and_instantiates()
{
    check(register_lut_grade(), "the element registers");

    GstElement *from_factory = gst_element_factory_make("lutgrade", nullptr);
    check(from_factory != nullptr, "the factory makes the registered element");
    if (from_factory != nullptr) {
        gst_object_unref(from_factory);
    }

    const ShaderPass pass = make_lut_pass(100.0f);
    const std::optional<std::string> source = fragment_source_lut_for(pass);
    GstStructure *uniforms = uniforms_for(pass);
    GstElement *element =
            source.has_value() ? make_lut_grade(*source, uniforms, *pass.lut) : nullptr;
    gst_structure_free(uniforms);
    check(element != nullptr, "make_lut_grade creates an element");
    if (element == nullptr) {
        return;
    }
    check(GST_IS_GL_FILTER(element), "the element is a GstGLFilter");

    guint size = 0;
    g_object_get(element, "lut-size", &size, nullptr);
    check(size == 2, "the element stores the LUT size");
    gst_object_unref(element);
}

// The headless EGL display + context the GL element borrows, answering the
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

// Runs one lutgrade pass at `strength` over a solid-red picture and returns
// the captured RGBA bytes.
struct RunResult
{
    bool eos = false;
    std::vector<std::uint8_t> bytes;
    bool captured = false;
};

RunResult run_lut_grade(GlContext &gl, float strength)
{
    RunResult result;
    const ShaderPass pass = make_lut_pass(strength);
    const std::optional<std::string> source = fragment_source_lut_for(pass);
    GstStructure *uniforms = uniforms_for(pass);
    GstElement *grade = source.has_value() ? make_lut_grade(*source, uniforms, *pass.lut) : nullptr;
    gst_structure_free(uniforms);

    GstElement *pipeline = gst_pipeline_new("test");
    if (pipeline == nullptr || grade == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&grade);
        return result;
    }

    // Solid red, uploaded to RGBA GLMemory, matching the production sandwich.
    GstElement *src = gst_element_factory_make("videotestsrc", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *upload = gst_element_factory_make("glupload", nullptr);
    GstElement *colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *download = gst_element_factory_make("gldownload", nullptr);
    GstElement *outconvert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *sink = gst_element_factory_make("appsink", "s");
    if (src == nullptr || convert == nullptr || upload == nullptr || colorconvert == nullptr
        || filter == nullptr || download == nullptr || outconvert == nullptr || sink == nullptr) {
        gst_object_unref(pipeline);
        return result;
    }
    gst_util_set_object_arg(G_OBJECT(src), "pattern", "red");
    g_object_set(src, "num-buffers", 1, nullptr);
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(sink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    GstCaps *sink_caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(sink, "caps", sink_caps, nullptr);
    gst_caps_unref(sink_caps);

    gst_bin_add_many(GST_BIN(pipeline), src, convert, upload, colorconvert, filter, grade, download,
                     outconvert, sink, nullptr);
    if (!gst_element_link_many(src, convert, upload, colorconvert, filter, grade, download,
                               outconvert, sink, nullptr)) {
        gst_object_unref(pipeline);
        return result;
    }

    CaptureLog capture;
    GstAppSinkCallbacks callbacks = { };
    callbacks.new_sample = on_capture_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, &capture, nullptr);

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

bool first_pixel_near(const std::vector<std::uint8_t> &bytes, int r, int g, int b, int tolerance)
{
    if (bytes.size() < 4) {
        return false;
    }
    return std::abs(static_cast<int>(bytes[0]) - r) <= tolerance
            && std::abs(static_cast<int>(bytes[1]) - g) <= tolerance
            && std::abs(static_cast<int>(bytes[2]) - b) <= tolerance;
}

void test_lut_grade_applies_the_table(GlContext &gl)
{
    const RunResult result = run_lut_grade(gl, 100.0f);
    check(result.eos, "the full-strength pipeline reaches EOS");
    if (!result.captured || result.bytes.empty()) {
        return;
    }
    // strength 100 replaces the picture with the table's green output.
    check(first_pixel_near(result.bytes, 0, 255, 0, 12),
          "the table's green output is applied at full strength");
}

void test_strength_mixes_toward_the_source(GlContext &gl)
{
    const RunResult result = run_lut_grade(gl, 0.0f);
    check(result.eos, "the zero-strength pipeline reaches EOS");
    if (!result.captured || result.bytes.empty()) {
        return;
    }
    // strength 0 keeps the untouched red picture.
    check(first_pixel_near(result.bytes, 255, 0, 0, 12),
          "strength 0 leaves the source red unchanged");
    check(!first_pixel_near(result.bytes, 0, 255, 0, 12),
          "the output is not the table's green at zero strength");
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

    test_lut_grade_applies_the_table(gl);
    test_strength_mixes_toward_the_source(gl);
}

} // namespace

int main()
{
    gst_init(nullptr, nullptr);
    test_lut_source_emits_the_3d_prelude();
    test_lut_source_refuses_without_strength();
    test_lut_source_refuses_pre_stages();
    test_registers_and_instantiates();
    run_gl_tests();

    return genesis::test::summary();
}
