// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <gst/app/gstappsink.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gst.h>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "adapters/engine/ges/effects/ShaderChain.h"
#include "core/ShaderPass.h"
#include "../support/Checks.h"
#include "../support/GlTest.h"

namespace {

using genesis::test::check;
using genesis::test::checks;
using genesis::test::failures;

// The GL-backed tests keep their own tally so the pure and GL sections report
// separately.
int gl_checks = 0;
int gl_failures = 0;

void gl_check(bool condition, const std::string &what)
{
    ++gl_checks;
    if (!condition) {
        ++gl_failures;
        std::printf("FAIL (gl): %s\n", what.c_str());
    }
}

using genesis::adapters::engine::ges::append_shader_chain;
using genesis::adapters::engine::ges::fragment_source_for;
using genesis::core::ShaderPass;

// A pass whose body is the given text after the prelude.
ShaderPass pass_with_body(std::string body)
{
    ShaderPass pass;
    pass.package = "test.pkg";
    pass.key = "test.pkg@1#0000000000000000";
    pass.source = std::make_shared<const std::string>(std::move(body));
    return pass;
}

// A pass whose resolver left `source` empty.
ShaderPass pass_without_body()
{
    ShaderPass pass;
    pass.package = "test.pkg";
    pass.key = "test.pkg@1#0000000000000000";
    return pass;
}

void test_the_prelude_is_emitted_verbatim()
{
    const auto source = fragment_source_for(pass_without_body());
    check(source.has_value(), "an empty-source pass still emits");
    check(source->find("precision mediump float") != std::string::npos,
          "the prelude declares precision");
    check(source->find("varying vec2 v_texcoord") != std::string::npos,
          "the prelude declares the varying");
    check(source->find("uniform sampler2D tex") != std::string::npos,
          "the prelude declares the one texture");
}

void test_an_empty_source_is_the_documented_identity()
{
    const auto source = fragment_source_for(pass_without_body());
    check(source.has_value(), "an empty source emits");
    check(source->find("void main () { gl_FragColor = texture2D (tex, v_texcoord); }")
                  != std::string::npos,
          "an empty source is the identity body");
    check(source->find("// identity: the resolver left the source empty") != std::string::npos,
          "the identity fallback is documented in the source");
    check(source->find("precision mediump float") != std::string::npos,
          "the identity still carries the prelude");
}

void test_a_body_is_appended_after_the_prelude()
{
    const std::string body = "void main () { gl_FragColor = vec4 (1.0, 0.0, 0.0, 1.0); }";
    const auto source = fragment_source_for(pass_with_body(body));
    check(source.has_value(), "a pass with a body emits");
    check(source->find(body) != std::string::npos, "the body is in the source");
    check(source->find(body) > source->find("uniform sampler2D tex"),
          "the body follows the prelude");
}

void test_a_second_sampler_is_refused()
{
    const std::string body = "uniform sampler2D tex2;"
                             " void main () { gl_FragColor = texture2D (tex2, v_texcoord); }";
    const auto source = fragment_source_for(pass_with_body(body));
    check(!source.has_value(), "a body declaring a second sampler2D is refused");
}

// A pass with parameters still emits the prelude and body: the resolved
// values must not corrupt or block the fragment source. This pass carries no
// `fields` (the values are set but the layout is empty), so no uniform
// declaration is emitted here; the declarations themselves are covered by
// test_uniform_declarations_are_emitted_between_prelude_and_body.
void test_a_pass_with_parameters_still_emits_prelude_and_body()
{
    ShaderPass pass =
            pass_with_body("void main () { gl_FragColor = texture2D (tex, v_texcoord); }");
    pass.params = std::vector<std::uint8_t>(16, 0);
    pass.values["amount"] = 0.5;

    const auto source = fragment_source_for(pass);
    check(source.has_value(), "a pass with parameters emits");
    check(source->find("precision mediump float") != std::string::npos,
          "a pass with parameters still carries the prelude");
    check(source->find("void main () { gl_FragColor = texture2D (tex, v_texcoord); }")
                  != std::string::npos,
          "a pass with parameters still carries the body");
}

// The uniform block: one declaration per field, using the field's GLSL type
// spelling and name, between the prelude and the body.
void test_uniform_declarations_are_emitted_between_prelude_and_body()
{
    ShaderPass pass =
            pass_with_body("void main () { gl_FragColor = texture2D (tex, v_texcoord); }");
    pass.fields = {
        { "amount", "float", 0, 4 },
        { "origin", "vec2", 8, 8 },
        { "tint", "vec4", 16, 16 },
        { "luma", "vec4[8]", 32, 128 },
    };

    const auto source = fragment_source_for(pass);
    check(source.has_value(), "a pass with fields emits");
    if (!source.has_value()) {
        return;
    }
    const std::string &text = *source;
    check(text.find("uniform float amount;") != std::string::npos, "a scalar declares a float");
    check(text.find("uniform vec2 origin;") != std::string::npos, "a point declares a vec2");
    check(text.find("uniform vec4 tint;") != std::string::npos, "a colour declares a vec4");
    check(text.find("uniform vec4[8] luma;") != std::string::npos, "a curve declares a vec4[8]");
    check(text.find("uniform float amount;") > text.find("uniform sampler2D tex"),
          "the declarations follow the prelude");
    check(text.find("void main") > text.find("uniform vec4[8] luma;"),
          "the declarations precede the body");
}

// ---- GL-backed section ---------------------------------------------------
//
// The following tests drive real glshader pipelines headlessly, using the EGL
// approach the other adapter suites use (FrameTextureTests.cpp). They answer
// the GL elements' NEED_CONTEXT messages on the bus and force the RGBA
// capsfilter the spike proved load-bearing.

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

// Answers the NEED_CONTEXT messages the GL elements post before negotiation,
// with the test's headless EGL display and context (the spike's finding).
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

// The head every GL pipeline starts from: a videotestsrc forced through the
// RGBA GLMemory capsfilter the spike proved load-bearing. `tail` is the
// capsfilter the shader chain links after; the pipeline owns every element.
struct Head
{
    GstElement *pipeline = nullptr;
    GstElement *tail = nullptr;
};

Head build_head(const char *pattern, int num_buffers)
{
    Head head;
    head.pipeline = gst_pipeline_new("test");
    GstElement *src = gst_element_factory_make("videotestsrc", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *upload = gst_element_factory_make("glupload", nullptr);
    GstElement *colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    if (head.pipeline == nullptr || src == nullptr || convert == nullptr || upload == nullptr
        || colorconvert == nullptr || filter == nullptr) {
        gst_clear_object(&head.pipeline);
        gst_clear_object(&src);
        gst_clear_object(&convert);
        gst_clear_object(&upload);
        gst_clear_object(&colorconvert);
        gst_clear_object(&filter);
        return head;
    }
    gst_util_set_object_arg(G_OBJECT(src), "pattern", pattern);
    g_object_set(src, "num-buffers", num_buffers, nullptr);
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(head.pipeline), src, convert, upload, colorconvert, filter, nullptr);
    if (!gst_element_link_many(src, convert, upload, colorconvert, filter, nullptr)) {
        gst_object_unref(head.pipeline);
        head.pipeline = nullptr;
        head.tail = nullptr;
        return head;
    }
    head.tail = filter;
    return head;
}

// The identity pass: the resolver leaves `source` empty, so the chain emits
// the documented identity body and no uniforms.
ShaderPass identity_pass()
{
    ShaderPass pass;
    pass.package = "test.identity";
    pass.key = "test.identity@1#0000000000000000";
    return pass;
}

// A tint pass controlled by one `amount` float: at 1.0 it inverts, at 0.0 it
// is the identity. The uniform declaration and the uploaded value both come
// from the field, so this pass also proves the uniform upload end-to-end.
ShaderPass invert_pass()
{
    ShaderPass pass = pass_with_body("void main () { vec4 c = texture2D (tex, v_texcoord);"
                                     " gl_FragColor = mix (c, vec4 (1.0 - c.rgb, c.a), amount); }");
    pass.fields = { { "amount", "float", 0, 4 } };
    pass.params = std::vector<std::uint8_t>(16, 0);
    const float one = 1.0f;
    std::memcpy(pass.params.data(), &one, sizeof(one));
    return pass;
}

// What the frame-inspection sink records, written by the streaming thread and
// read after the pipeline is torn down.
struct FrameLog
{
    int frames = 0;
    int gl_memory = 0;
    bool texture_2d = false;
    guint texture_id = 0;
};

GstFlowReturn on_gl_sample(GstAppSink *sink, gpointer user_data)
{
    auto *log = static_cast<FrameLog *>(user_data);
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
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        const char *target = gst_structure_get_string(structure, "texture-target");
        if (target != nullptr && g_strcmp0(target, "2D") == 0) {
            log->texture_2d = true;
        }
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (buffer != nullptr) {
        GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
        if (memory != nullptr && gst_is_gl_memory(memory)) {
            log->texture_id = gst_gl_memory_get_texture_id(GST_GL_MEMORY_CAST(memory));
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// The downloaded RGBA bytes, written by the streaming thread's appsink
// callback and read back after the pipeline has gone EOS.
struct RgbaLog
{
    std::vector<std::uint8_t> bytes;
    bool captured = false;
};

GstFlowReturn on_capture_sample(GstAppSink *sink, gpointer user_data)
{
    auto *log = static_cast<RgbaLog *>(user_data);
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

// Runs the chain for `passes` against a one-buffer red videotestsrc, downloads
// the result to system memory, and returns the RGBA bytes captured by the
// appsink. nullopt when the pipeline cannot be built or run to EOS.
std::optional<std::vector<std::uint8_t>> capture_rgba(GlContext &gl,
                                                      const std::vector<ShaderPass> &passes)
{
    Head head = build_head("red", 1);
    if (head.pipeline == nullptr) {
        return std::nullopt;
    }

    GstElement *tail = append_shader_chain(head.tail, passes);
    if (tail == nullptr) {
        gst_object_unref(head.pipeline);
        return std::nullopt;
    }

    GstElement *download = gst_element_factory_make("gldownload", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", nullptr);
    if (download == nullptr || convert == nullptr || appsink == nullptr) {
        gst_clear_object(&download);
        gst_clear_object(&convert);
        gst_clear_object(&appsink);
        gst_object_unref(head.pipeline);
        return std::nullopt;
    }
    GstCaps *sink_caps = gst_caps_from_string("video/x-raw,format=RGBA");
    g_object_set(appsink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, "caps", sink_caps,
                 nullptr);
    gst_caps_unref(sink_caps);
    gst_bin_add_many(GST_BIN(head.pipeline), download, convert, appsink, nullptr);
    if (!gst_element_link_many(tail, download, convert, appsink, nullptr)) {
        gst_object_unref(head.pipeline);
        return std::nullopt;
    }

    RgbaLog log;
    GstAppSinkCallbacks callbacks = { };
    callbacks.new_sample = on_capture_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, &log, nullptr);

    GstBus *bus = gst_element_get_bus(head.pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);
    gst_object_unref(bus);

    gst_element_set_state(head.pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (message != nullptr) {
        gst_message_unref(message);
    }
    gst_element_set_state(head.pipeline, GST_STATE_NULL);
    gst_object_unref(head.pipeline);
    if (!log.captured) {
        return std::nullopt;
    }
    return log.bytes;
}

void test_one_pass_chain_delivers_gl_frames(GlContext &gl)
{
    Head head = build_head("red", 30);
    gl_check(head.pipeline != nullptr, "the head pipeline builds");
    if (head.pipeline == nullptr) {
        return;
    }

    GstElement *tail = append_shader_chain(head.tail, { identity_pass() });
    gl_check(tail != nullptr, "a one-pass chain links");
    if (tail == nullptr) {
        gst_object_unref(head.pipeline);
        return;
    }

    GstElement *appsink = gst_element_factory_make("appsink", "sink");
    gl_check(appsink != nullptr, "the frame-inspection appsink is created");
    if (appsink == nullptr) {
        gst_object_unref(head.pipeline);
        return;
    }
    g_object_set(appsink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    gst_bin_add(GST_BIN(head.pipeline), appsink);
    const gboolean linked = gst_element_link(tail, appsink);
    gl_check(linked != FALSE, "the chain links to the appsink");
    if (!linked) {
        gst_object_unref(head.pipeline);
        return;
    }

    FrameLog log;
    GstAppSinkCallbacks callbacks = { };
    callbacks.new_sample = on_gl_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, &log, nullptr);

    GstBus *bus = gst_element_get_bus(head.pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);
    gst_object_unref(bus);

    gst_element_set_state(head.pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (message != nullptr) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            gl_check(false,
                     std::string("the pipeline reaches EOS: ")
                             + (error != nullptr ? error->message : "?"));
            if (error != nullptr) {
                g_error_free(error);
            }
            g_free(debug);
        }
        gst_message_unref(message);
    }
    gst_element_set_state(head.pipeline, GST_STATE_NULL);

    gl_check(log.frames > 0, "at least one frame arrives");
    gl_check(log.gl_memory == log.frames, "every frame is GLMemory");
    gl_check(log.texture_2d, "the frames carry texture-target=2D");
    gl_check(log.texture_id != 0, "the frame carries a non-zero texture id");

    gst_object_unref(head.pipeline);
}

void test_two_pass_chain_output_differs(GlContext &gl)
{
    const auto one = capture_rgba(gl, { identity_pass() });
    const auto two = capture_rgba(gl, { identity_pass(), invert_pass() });
    gl_check(one.has_value() && !one->empty(), "the one-pass output captures");
    gl_check(two.has_value() && !two->empty(), "the two-pass output captures");
    if (!one.has_value() || !two.has_value() || one->empty() || two->empty()) {
        return;
    }
    gl_check(*one != *two, "the two-pass output differs from the one-pass");
    // The source is solid red; the one-pass identity keeps it red (R > B), and
    // the second pass inverts it to cyan (B > R) only if its `amount` uniform
    // was actually uploaded as 1.0.
    gl_check((*one)[0] > (*one)[2], "the identity keeps the red source red");
    gl_check((*two)[2] > (*two)[0], "the second pass inverts red to cyan (uniform uploaded)");
}

void test_pre_stages_are_refused()
{
    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    gst_bin_add(GST_BIN(pipeline), sink);

    ShaderPass pass = identity_pass();
    pass.stages = { { "blur", { 1, 1 } } };

    GstElement *tail = append_shader_chain(sink, { pass });
    gl_check(tail == nullptr, "a pass with pre-stages is refused");

    gst_object_unref(pipeline);
}

void test_a_second_sampler_aborts_the_whole_chain()
{
    GstElement *pipeline = gst_pipeline_new(nullptr);
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    gst_bin_add(GST_BIN(pipeline), sink);

    ShaderPass bad =
            pass_with_body("uniform sampler2D tex2;"
                           " void main () { gl_FragColor = texture2D (tex2, v_texcoord); }");
    // A valid first pass must not mask the refused second: the whole chain
    // aborts, so no glshader is linked and nothing is half-built.
    GstElement *tail = append_shader_chain(sink, { identity_pass(), bad });
    gl_check(tail == nullptr, "a second-sampler pass aborts the whole chain");

    gst_object_unref(pipeline);
}

void run_gl_tests()
{
    gst_init(nullptr, nullptr);

    // The refusal tests need no GL context: append_shader_chain refuses before
    // any glshader is created, so they run regardless of display availability.
    test_pre_stages_are_refused();
    test_a_second_sampler_aborts_the_whole_chain();

    GlContext gl;
    gl.display = genesis::test::make_egl_display();
    gl_check(gl.display != nullptr, "a headless EGL display is available");
    if (gl.display == nullptr) {
        return;
    }
    gl.context = gst_gl_context_new(gl.display);
    GError *error = nullptr;
    if (!gst_gl_context_create(gl.context, nullptr, &error)) {
        gl_check(false,
                 std::string("a headless GL context creates: ")
                         + (error != nullptr ? error->message : "unknown"));
        g_clear_error(&error);
        return;
    }

    test_one_pass_chain_delivers_gl_frames(gl);
    test_two_pass_chain_output_differs(gl);
}

} // namespace

int main()
{
    // Pure tests: GLSL text assembly, no GStreamer or GL needed.
    test_the_prelude_is_emitted_verbatim();
    test_an_empty_source_is_the_documented_identity();
    test_a_body_is_appended_after_the_prelude();
    test_a_second_sampler_is_refused();
    test_a_pass_with_parameters_still_emits_prelude_and_body();
    test_uniform_declarations_are_emitted_between_prelude_and_body();
    std::printf("pure: %d checks, %d failures\n", checks, failures);

    // GL-backed tests: build and drive real glshader pipelines headlessly.
    run_gl_tests();
    std::printf("gl: %d checks, %d failures\n", gl_checks, gl_failures);

    const int total = failures + gl_failures;
    std::printf("%d checks, %d failures\n", checks + gl_checks, total);
    return total == 0 ? 0 : 1;
}
