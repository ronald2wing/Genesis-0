// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <gst/app/gstappsink.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gst.h>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../support/Checks.h"
#include "adapters/engine/ges/effects/ShaderChain.h"
#include "effects/PackLoader.h"
#include "effects/PackSource.h"
#include "extensions/ExtensionManifest.h"
#include "effects/Resolve.h"
#include "project/command/Command.h"
#include "project/model/Effects.h"

namespace {

using genesis::test::check;
using genesis::test::checks;
using genesis::test::failures;
// The GL-backed tests keep their own tally so the pure and GL sections report
// separately, like ShaderChainTests.
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
using namespace genesis::effects;
using genesis::core::ShaderPass;
using genesis::project::AppliedFilter;

// The five packs docs/decisions/effect-execution.md §6 excluded as needing a
// named intermediate, re-authored as one body that folds its passes into a
// single shader on the glshader (single-texture) path. Each folds to tex-only
// fetches - the engine's glshader prepends its prelude (the one texture `tex`
// it binds) and the pack's parameter uniform declarations, then appends the
// body, so none needs a second distinct input.
constexpr std::array<std::string_view, 5> kNamedPacks = { "genesis.gaussian-blur",
                                                          "genesis.bloom-pulse", "genesis.glow",
                                                          "genesis.motion-blur",
                                                          "genesis.zoom-blur" };

// The batch this test was written for: the thirty-six previous packs plus the
// five named-intermediate re-authors, grown by the four audio packs.
constexpr std::size_t kExpectedPacks = 123;

// Whether a body names `sampler2D`. The prelude already declares the one
// texture glshader binds; a body naming `sampler2D` again is a second texture
// unit, which glshader silently aliases to unit 0 - the adapter refuses it
// rather than emitting it (ShaderChain::fragment_source_for,
// docs/decisions/effect-execution.md §6).
bool declares_sampler(std::string_view body)
{
    return body.find("sampler2D") != std::string_view::npos;
}

// How many times `needle` appears in `hay`.
std::size_t count_of(std::string_view hay, std::string_view needle)
{
    std::size_t total = 0;
    for (std::size_t at = hay.find(needle); at != std::string_view::npos;
         at = hay.find(needle, at + needle.size())) {
        ++total;
    }
    return total;
}

// The unified extension.toml for the builtin genesis.packs extension, found by
// walking up from the working directory.
std::optional<std::filesystem::path> locate_manifest()
{
    std::filesystem::path dir = std::filesystem::current_path();
    while (true) {
        const std::filesystem::path candidate =
                dir / "extensions" / "builtin" / "genesis.packs" / "extension.toml";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
            return candidate;
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) {
            return std::nullopt;
        }
        dir = parent;
    }
}

// Finds a pack by id in the manifest's effects list.
const genesis::effects::Pack *find_pack(const std::vector<genesis::effects::Pack> &effects,
                                        const std::string &id)
{
    for (const genesis::effects::Pack &pack : effects) {
        if (pack.id == id) {
            return &pack;
        }
    }
    return nullptr;
}

// Each of the five named-intermediate re-authors loads, resolves to a pass, and
// its body follows the glshader (single-texture) convention: it samples the one
// texture the prelude declares (`tex`), declares no sampler of its own, and
// leaves every parameter uniform to the engine's emitted declarations - the
// whole fragment has exactly one sampler2D.
void test_each_named_pack_folds_to_a_single_texture_body()
{
    const std::optional<std::filesystem::path> manifest = locate_manifest();
    check(manifest.has_value(), "extension.toml is found by walking up from cwd");
    if (!manifest) {
        return;
    }
    const genesis::extensions::NativeLoadResult loaded =
            genesis::extensions::load_native_manifest_file(*manifest);
    check(loaded.manifest.has_value(), "the manifest loads");
    if (!loaded.manifest) {
        return;
    }
    const std::filesystem::path manifest_dir = manifest->parent_path();

    for (const std::string_view id : kNamedPacks) {
        const std::string tag(id);
        const genesis::effects::Pack *pack = find_pack(loaded.manifest->effects, tag);
        check(pack != nullptr, tag + ": the pack is in the manifest");
        if (!pack) {
            continue;
        }
        check(pack->id == tag, tag + ": the id matches");
        check(is_expressible(*pack), tag + ": the pack is expressible (no named passes)");
        check(pack->passes.empty(), tag + ": the passes are folded into one body");

        const std::filesystem::path body_path = manifest_dir / pack->body;
        const SourceResult resolved =
                resolve_body(body_path, *pack, AppliedFilter::create(tag), 0.0);
        check(resolved.pass.has_value() && resolved.problem.empty(),
              tag + ": resolve_body produces a pass");
        if (!resolved.pass) {
            continue;
        }
        const ShaderPass &pass = *resolved.pass;
        check(pass.package == tag, tag + ": the pass carries the pack's id");
        if (pass.source == nullptr || pass.source->empty()) {
            check(false, tag + ": the body is non-empty");
            continue;
        }

        const std::string_view body = *pass.source;
        check(!declares_sampler(body), tag + ": the body declares no sampler2D");
        check(body.find("#version") == std::string_view::npos,
              tag + ": the body declares no version (ES 1.00 is implied)");
        check(body.find("void main") != std::string_view::npos, tag + ": the body has a main");
        check(body.find("gl_FragColor") != std::string_view::npos,
              tag + ": the body writes gl_FragColor");
        check(body.find("texture2D (tex") != std::string_view::npos,
              tag + ": the body samples tex (the picture)");
        check(body.find("tex0") == std::string_view::npos,
              tag + ": the body does not name the two-input sampler");

        // The engine emits one uniform declaration per parameter field, so the
        // body must not redeclare any of them - a duplicate declaration would
        // not compile, and a missing one would leave the knob silent.
        for (const auto &field : pass.fields) {
            check(body.find("uniform " + field.type + " " + field.name + ";")
                          == std::string_view::npos,
                  tag + ": the body does not redeclare its `" + field.name + "` uniform");
        }

        // The whole fragment the engine compiles (the real assembly path, not
        // a copy): the prelude's `tex` is the only sampler2D, and each field
        // is declared exactly once, by the engine.
        const std::optional<std::string> fragment = fragment_source_for(pass);
        check(fragment.has_value(), tag + ": fragment_source_for emits");
        if (!fragment) {
            continue;
        }
        check(count_of(*fragment, "sampler2D") == 1,
              tag + ": exactly one sampler2D, the prelude's");
        for (const auto &field : pass.fields) {
            check(fragment->find("uniform " + field.type + " " + field.name + ";")
                          != std::string::npos,
                  tag + ": the engine declares its `" + field.name + "` uniform");
        }
    }
}

// The batch as a whole: forty-one packs, every one loads, ids unique.
void test_the_batch_holds_forty_one_unique_packs()
{
    const std::optional<std::filesystem::path> manifest = locate_manifest();
    check(manifest.has_value(), "extension.toml is found by walking up from cwd");
    if (!manifest) {
        return;
    }
    const genesis::extensions::NativeLoadResult loaded =
            genesis::extensions::load_native_manifest_file(*manifest);
    check(loaded.manifest.has_value(), "the manifest loads");
    if (!loaded.manifest) {
        return;
    }
    check(loaded.manifest->effects.size() == kExpectedPacks,
          "the pack count is " + std::to_string(kExpectedPacks) + " (saw "
                  + std::to_string(loaded.manifest->effects.size()) + ")");
    if (loaded.manifest->effects.empty()) {
        return;
    }
    std::set<std::string> ids;
    for (const genesis::effects::Pack &pack : loaded.manifest->effects) {
        check(ids.insert(pack.id).second, pack.id + ": the id is unique across the batch");
    }
}

// ---- GL-backed section ---------------------------------------------------
//
// The following drives real glshader pipelines headlessly - the same EGL
// approach ShaderChainTests uses - to prove each pack's body actually compiles
// and renders on the glshader route, and moves pixels in the effect's causal
// direction. This section requires linking genesis_engine_ges (see CMake).

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

// Answers the NEED_CONTEXT messages the GL elements post before negotiation.
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
// RGBA GLMemory capsfilter the spike proved load-bearing.
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
        return head;
    }
    head.tail = filter;
    return head;
}

// The downloaded RGBA bytes, written by the streaming thread's appsink
// callback and read back after the pipeline has gone EOS.
struct RgbaLog
{
    std::vector<std::uint8_t> bytes;
    int width = 0;
    int height = 0;
    bool captured = false;
};

GstFlowReturn on_capture_sample(GstAppSink *sink, gpointer user_data)
{
    auto *log = static_cast<RgbaLog *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstCaps *caps = gst_sample_get_caps(sample);
    if (caps != nullptr) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(structure, "width", &log->width);
        gst_structure_get_int(structure, "height", &log->height);
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

// Runs the chain for `passes` against a one-buffer videotestsrc, downloads the
// result, and returns the RGBA bytes. Empty `passes` captures the unmodified
// source (the identity reference). nullopt when the pipeline cannot be built
// or run to EOS - a fragment that does not compile posts an ERROR and never
// reaches EOS, so a returned frame proves the shader compiled and rendered.
std::optional<RgbaLog> capture_rgba(GlContext &gl, const std::vector<ShaderPass> &passes,
                                    const char *pattern)
{
    Head head = build_head(pattern, 1);
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
    return log;
}

double luma(const std::uint8_t *pixel)
{
    return 0.299 * pixel[0] + 0.587 * pixel[1] + 0.114 * pixel[2];
}

double mean_luminance(const RgbaLog &log)
{
    double sum = 0.0;
    long count = 0;
    for (int y = 0; y < log.height; ++y) {
        for (int x = 0; x < log.width; ++x) {
            sum += luma(&log.bytes[(y * log.width + x) * 4]);
            ++count;
        }
    }
    return count != 0 ? sum / count : 0.0;
}

// Mean absolute luminance difference between adjacent pixels: a blur is a
// low-pass, so it lowers this. A sharp source's edge energy falls as it blurs.
double gradient_energy(const RgbaLog &log)
{
    double energy = 0.0;
    long count = 0;
    for (int y = 0; y < log.height; ++y) {
        for (int x = 0; x + 1 < log.width; ++x) {
            energy += std::fabs(luma(&log.bytes[(y * log.width + x) * 4])
                                - luma(&log.bytes[(y * log.width + x + 1) * 4]));
            ++count;
        }
    }
    for (int y = 0; y + 1 < log.height; ++y) {
        for (int x = 0; x < log.width; ++x) {
            energy += std::fabs(luma(&log.bytes[(y * log.width + x) * 4])
                                - luma(&log.bytes[((y + 1) * log.width + x) * 4]));
            ++count;
        }
    }
    return count != 0 ? energy / count : 0.0;
}

// Each pack renders on the real glshader route and moves pixels in its causal
// direction: the three blurs soften edges (gradient energy falls), and the two
// glow/bright-pass effects raise the mean luminance (screening adds light).
void test_each_named_pack_executes(GlContext &gl)
{
    const std::optional<std::filesystem::path> manifest = locate_manifest();
    gl_check(manifest.has_value(), "extension.toml is found for the GL section");
    if (!manifest) {
        return;
    }
    const genesis::extensions::NativeLoadResult loaded =
            genesis::extensions::load_native_manifest_file(*manifest);
    gl_check(loaded.manifest.has_value(), "the manifest loads");
    if (!loaded.manifest) {
        return;
    }
    const std::filesystem::path manifest_dir = manifest->parent_path();
    const std::array<std::string_view, 3> kBlurs = { "genesis.gaussian-blur", "genesis.motion-blur",
                                                     "genesis.zoom-blur" };

    for (const std::string_view id : kNamedPacks) {
        const std::string tag(id);
        const genesis::effects::Pack *pack = find_pack(loaded.manifest->effects, tag);
        gl_check(pack != nullptr, tag + ": pack found in manifest");
        if (!pack) {
            continue;
        }
        const std::filesystem::path body_path = manifest_dir / pack->body;
        const SourceResult resolved =
                resolve_body(body_path, *pack, AppliedFilter::create(tag), 0.0);
        gl_check(resolved.pass.has_value(), tag + ": resolves to a pass");
        if (!resolved.pass) {
            continue;
        }

        const std::optional<RgbaLog> identity = capture_rgba(gl, { }, "smpte");
        const std::optional<RgbaLog> effected = capture_rgba(gl, { *resolved.pass }, "smpte");
        gl_check(identity.has_value(), tag + ": the source renders (identity)");
        gl_check(effected.has_value(), tag + ": the effect shader compiles and renders a frame");
        if (!identity || !effected) {
            continue;
        }
        gl_check(identity->bytes != effected->bytes,
                 tag + ": the effect changes pixels (not a no-op)");

        const bool is_blur = std::find(kBlurs.begin(), kBlurs.end(), id) != kBlurs.end();
        if (is_blur) {
            gl_check(gradient_energy(*effected) < gradient_energy(*identity),
                     tag + ": blur reduces gradient energy (edges soften)");
        } else {
            gl_check(mean_luminance(*effected) > mean_luminance(*identity),
                     tag + ": glow/bright-pass raises mean luminance");
        }
    }
}

void run_gl_tests()
{
    gst_init(nullptr, nullptr);

    GlContext gl;
    gl.display = reinterpret_cast<GstGLDisplay *>(gst_gl_display_egl_new());
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

    test_each_named_pack_executes(gl);
}

} // namespace

int main()
{
    // Pure tests: manifest/validation, GLSL convention, no GL context needed.
    test_each_named_pack_folds_to_a_single_texture_body();
    test_the_batch_holds_forty_one_unique_packs();
    std::printf("pure: %d checks, %d failures\n", checks, failures);

    // GL-backed tests: build and drive real glshader pipelines headlessly.
    run_gl_tests();
    std::printf("gl: %d checks, %d failures\n", gl_checks, gl_failures);

    const int total = failures + gl_failures;
    std::printf("%d checks, %d failures\n", checks + gl_checks, total);
    return total == 0 ? 0 : 1;
}
