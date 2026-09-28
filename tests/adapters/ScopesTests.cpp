// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <gst/app/gstappsink.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
#include <gst/gl/gstglmemory.h>
#include <gst/gst.h>
#include <gst/video/video-info.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "adapters/engine/EngineSession.h"
#include "adapters/engine/ges/scopes/Scopes.h"

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

namespace engine = genesis::adapters::engine;
namespace scopes_ns = genesis::adapters::engine::ges;

using scopes_ns::kScopeColumns;
using scopes_ns::kScopeLevels;
using scopes_ns::ScopeFrame;

// ---- pure reduction (no GL) ---------------------------------------------

// A tightly packed RGBA8 buffer, filled by `fill` for each pixel.
std::vector<std::uint8_t> make_rgba(std::uint32_t width, std::uint32_t height,
                                    void (*fill)(std::uint8_t *, std::uint32_t, std::uint32_t))
{
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t *px = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            fill(px, x, y);
        }
    }
    return rgba;
}

void fill_solid_red(std::uint8_t *px, std::uint32_t, std::uint32_t)
{
    px[0] = 255;
    px[1] = 0;
    px[2] = 0;
    px[3] = 255;
}

void fill_gradient(std::uint8_t *px, std::uint32_t x, std::uint32_t)
{
    const std::uint8_t v = static_cast<std::uint8_t>(x % 256);
    px[0] = v;
    px[1] = v;
    px[2] = v;
    px[3] = 255;
}

void test_solid_red_histogram_peaks()
{
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 32;
    const auto rgba = make_rgba(kWidth, kHeight, fill_solid_red);
    const ScopeFrame frame = scopes_ns::reduce_scopes(rgba.data(), kWidth, kHeight);
    const std::uint32_t total = kWidth * kHeight;

    check(frame.histogram.red[255] == total, "pure red: red peak at 255");
    check(frame.histogram.green[0] == total, "pure red: green is all zero");
    check(frame.histogram.blue[0] == total, "pure red: blue is all zero");
    // Rec.709 luma of (255,0,0) is 0.2126*255 = 54.21 -> bin 54.
    std::uint32_t luma_total = 0;
    for (std::uint32_t i = 0; i < kScopeLevels; ++i) {
        luma_total += frame.histogram.luma[i];
    }
    check(luma_total == total, "pure red: luma histogram sums to the frame");
    check(frame.histogram.luma[54] == total, "pure red: luma peaks at Rec.709 54");
}

void test_gradient_spreads_not_spikes()
{
    constexpr std::uint32_t kWidth = 256;
    constexpr std::uint32_t kHeight = 16;
    const auto rgba = make_rgba(kWidth, kHeight, fill_gradient);
    const ScopeFrame frame = scopes_ns::reduce_scopes(rgba.data(), kWidth, kHeight);

    // A horizontal ramp touches many red bins, not a single spike.
    std::uint32_t non_zero_bins = 0;
    for (std::uint32_t i = 0; i < kScopeLevels; ++i) {
        if (frame.histogram.red[i] != 0) {
            ++non_zero_bins;
        }
    }
    check(non_zero_bins >= kWidth, "a ramp spreads across many red bins");

    // The waveform's max tracks the ramp: column 0 is dark, the last column
    // is bright.
    const auto &wave = frame.waveform;
    check(wave.columns == kScopeColumns, "waveform reports 256 columns");
    check(wave.red_max[0] < wave.red_max[kScopeColumns - 1],
          "waveform max rises left to right along the ramp");
}

void test_waveform_flat_on_solid()
{
    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 32;
    const auto rgba = make_rgba(kWidth, kHeight, fill_solid_red);
    const ScopeFrame frame = scopes_ns::reduce_scopes(rgba.data(), kWidth, kHeight);

    // A solid colour gives every *touched* column a flat min==max == the
    // colour. width 128 maps onto 256 columns as col = x*256/128 = 2x, so only
    // the even columns receive a pixel; the untouched columns keep the empty
    // sentinel (min=255, max=0) and are not "flat". The reducer is correct:
    // the old loop asserted flatness on columns that had never seen a pixel.
    std::uint32_t touched = 0;
    bool flat = true;
    for (std::uint32_t c = 0; c < kScopeColumns; ++c) {
        if (frame.waveform.red_min[c] == 255 && frame.waveform.red_max[c] == 0) {
            continue; // untouched column: the empty sentinel
        }
        ++touched;
        flat = flat && frame.waveform.red_min[c] == 255 && frame.waveform.red_max[c] == 255;
    }
    check(touched == kWidth, "a solid colour touches one waveform column per source column");
    check(flat, "a solid colour is flat (min==max==255) in every touched column");
}

void test_vectorscope_red_lands_top_left()
{
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 64;
    const auto rgba = make_rgba(kWidth, kHeight, fill_solid_red);
    const ScopeFrame frame = scopes_ns::reduce_scopes(rgba.data(), kWidth, kHeight);
    const auto &vec = frame.vectorscope;

    check(vec.size == kScopeLevels, "vectorscope reports 256x256");
    check(vec.density.size() == kScopeLevels * kScopeLevels, "vectorscope density is 256*256");

    // Saturated red is a single point: all density in one bin.
    std::uint32_t non_zero = 0;
    std::uint32_t peak_row = 0;
    std::uint32_t peak_col = 0;
    for (std::uint32_t r = 0; r < kScopeLevels; ++r) {
        for (std::uint32_t c = 0; c < kScopeLevels; ++c) {
            if (vec.density[static_cast<std::size_t>(r) * kScopeLevels + c] != 0) {
                ++non_zero;
                peak_row = r;
                peak_col = c;
            }
        }
    }
    check(non_zero == 1, "saturated red lands in exactly one vectorscope bin");
    check(peak_row == 0, "red saturates to the top row (+Cr)");
    check(peak_col < kScopeLevels / 2, "red lands in the left half (-Cb)");
}

// ---- GL-backed section ---------------------------------------------------

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

// What the readback callback records. The streaming thread writes it; the test
// reads it after the pipeline has torn down.
struct ReadbackLog
{
    std::mutex mutex;
    int frames = 0;
};

GstFlowReturn on_readback_sample(GstAppSink *sink, gpointer user_data)
{
    auto *log = static_cast<ReadbackLog *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstCaps *caps = gst_sample_get_caps(sample);
    if (buffer != nullptr && caps != nullptr) {
        GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
        if (memory != nullptr && gst_is_gl_memory(memory)) {
            const guint texture_id = gst_gl_memory_get_texture_id(GST_GL_MEMORY_CAST(memory));
            const GstStructure *structure = gst_caps_get_structure(caps, 0);
            gint width = 0;
            gint height = 0;
            gst_structure_get_int(structure, "width", &width);
            gst_structure_get_int(structure, "height", &height);

            GstBuffer *keep = gst_buffer_ref(buffer);
            std::shared_ptr<void> keep_alive(static_cast<void *>(keep), [](void *p) {
                gst_buffer_unref(static_cast<GstBuffer *>(p));
            });

            auto *scopes =
                    static_cast<scopes_ns::Scopes *>(g_object_get_data(G_OBJECT(sink), "scopes"));
            // Drive the async readback; the completed scopes are polled via
            // Scopes::latest() after the pipeline, not inspected here.
            scopes->read_back(texture_id, width, height, std::move(keep_alive));
            std::lock_guard<std::mutex> lock(log->mutex);
            ++log->frames;
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// Builds videotestsrc -> videoconvert -> glupload -> glcolorconvert ->
// capsfilter(RGBA GLMemory) -> appsink and drives it to EOS, running
// `on_readback_sample` per frame with `scopes` attached to the appsink.
// `drain` (if given) runs after EOS while the pipeline is still alive, so the
// caller can poll the last readback before teardown - tearing the pipeline down
// first races the worker's final readback. Returns the log of what the callback
// saw, or nullptr if the pipeline cannot build or run.
std::unique_ptr<ReadbackLog> run_readback_pipeline(GlContext &gl, scopes_ns::Scopes &scopes,
                                                   const char *pattern, int num_buffers,
                                                   const std::function<void()> &drain = { })
{
    GstElement *pipeline = gst_pipeline_new("scopes");
    GstElement *src = gst_element_factory_make("videotestsrc", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *upload = gst_element_factory_make("glupload", nullptr);
    GstElement *colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", nullptr);
    if (pipeline == nullptr || src == nullptr || convert == nullptr || upload == nullptr
        || colorconvert == nullptr || filter == nullptr || appsink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&src);
        gst_clear_object(&convert);
        gst_clear_object(&upload);
        gst_clear_object(&colorconvert);
        gst_clear_object(&filter);
        gst_clear_object(&appsink);
        return nullptr;
    }

    gst_util_set_object_arg(G_OBJECT(src), "pattern", pattern);
    g_object_set(src, "num-buffers", num_buffers, nullptr);
    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(appsink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    g_object_set_data(G_OBJECT(appsink), "scopes", &scopes);

    gst_bin_add_many(GST_BIN(pipeline), src, convert, upload, colorconvert, filter, appsink,
                     nullptr);
    if (!gst_element_link_many(src, convert, upload, colorconvert, filter, appsink, nullptr)) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    auto log = std::make_unique<ReadbackLog>();
    GstAppSinkCallbacks callbacks = { };
    callbacks.new_sample = on_readback_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, log.get(), nullptr);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
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
    // Drain while the pipeline is still alive: tearing it down first destroys
    // the sibling GL context the worker's final readback still needs, so the
    // caller's last poll must happen before teardown.
    if (drain) {
        drain();
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return log;
}

void test_read_scopes_nullopt_before_readback(const GlContext &gl)
{
    // An inert Scopes (no GL context) never hangs and never produces a frame.
    // Both constructors stay inert when given nothing: the legacy raw-handles
    // form, and the preferred pipeline-context form with a null context.
    {
        scopes_ns::Scopes inert(engine::GlHandles{ });
        check(!scopes_ns::read_scopes(inert).has_value(), "an inert Scopes reports no frame");
        check(!inert.read_back(1, 16, 16).has_value(),
              "an inert Scopes read_back reports no frame");
    }
    {
        scopes_ns::Scopes inert(static_cast<GstGLContext *>(nullptr));
        check(!scopes_ns::read_scopes(inert).has_value(), "a null-context Scopes reports no frame");
        check(!inert.read_back(1, 16, 16).has_value(),
              "a null-context Scopes read_back reports no frame");
    }

    // A live Scopes (on the pipeline's context) must answer immediately
    // (nullopt) before any readback.
    scopes_ns::Scopes scopes(gl.context);
    check(!scopes_ns::read_scopes(scopes).has_value(),
          "read_scopes is nullopt before any readback");
    check(!scopes.latest().has_value(), "latest is nullopt before any readback");
}

void test_readback_solid_red(GlContext &gl)
{
    scopes_ns::Scopes scopes(gl.context);

    // The readback is asynchronous and one frame behind, so poll for the first
    // completed frame while the pipeline is still alive. The pixel assertions
    // below are unconditional: a readback that returns black instead of the
    // painted red is a real regression (a missing producer fence, a wrong
    // target, or an unfenced sibling context) and must fail - not be retried
    // until red happens to appear.
    std::optional<ScopeFrame> frame_opt;
    const auto drain = [&scopes, &frame_opt] {
        for (int i = 0; i < 400; ++i) {
            auto candidate = scopes.latest();
            if (candidate.has_value()) {
                frame_opt = candidate;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    const auto log = run_readback_pipeline(gl, scopes, "red", 30, drain);
    gl_check(log != nullptr, "the red readback pipeline builds and runs");
    if (log == nullptr) {
        return;
    }
    gl_check(log->frames > 0, "the readback pipeline delivers frames");

    // One diagnostic line distinguishing blank from wrong-target from
    // missing-sync, then the strict pixel assertions.
    const scopes_ns::ReadbackStats stats = scopes.readback_stats();
    std::printf("readback stats: submitted=%llu processed=%llu completed=%llu "
                "failed=%llu last_target=0x%x fbo_complete=%d sync_meta=%d "
                "sync_shared=%d\n",
                static_cast<unsigned long long>(stats.submitted),
                static_cast<unsigned long long>(stats.processed),
                static_cast<unsigned long long>(stats.completed),
                static_cast<unsigned long long>(stats.failed), stats.last_target,
                stats.last_fbo_complete ? 1 : 0, stats.last_sync_meta ? 1 : 0,
                stats.sync_context_shared ? 1 : 0);

    gl_check(stats.completed > 0, "at least one readback completes");
    gl_check(frame_opt.has_value(), "a completed readback stores a scope frame");
    if (!frame_opt.has_value()) {
        return;
    }

    // Say what the readback actually contained. A CI runner once reported only
    // "red does not peak at 255", which left the next reader guessing whether
    // the frame was black, transposed, or a different frame entirely. One line
    // of output answers that.
    {
        std::uint32_t total = 0;
        std::uint32_t peak_bin = 0;
        std::uint32_t peak_count = 0;
        for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
            const std::uint32_t count = frame_opt->histogram.red[level];
            total += count;
            if (count > peak_count) {
                peak_count = count;
                peak_bin = level;
            }
        }
        std::printf("readback red histogram: %u px, peak at bin %u (%u of them)\n", total, peak_bin,
                    peak_count);
    }

    const ScopeFrame &frame = *frame_opt;

    // The red videotestsrc frame is solid (255,0,0): the histogram peaks are
    // exact, whatever the pipeline's frame size.
    std::uint32_t pixels = 0;
    for (std::uint32_t i = 0; i < kScopeLevels; ++i) {
        pixels += frame.histogram.red[i];
    }
    gl_check(pixels > 0, "the readback histogram counts real pixels");

    // Scopes waits on the producer buffer's GstGLSyncMeta before reading the
    // shared texture, so the readback sees what the pipeline painted even
    // though the pipeline renders on a sibling context. A blank readback here
    // is a real regression (a missing sync point or a wrong target), not a
    // platform quirk, and must fail.
    gl_check(frame.histogram.red[255] == pixels, "solid red: every pixel peaks at red 255");
    gl_check(frame.histogram.green[0] == pixels, "solid red: every pixel is green 0");
    gl_check(frame.histogram.blue[0] == pixels, "solid red: every pixel is blue 0");

    // Vectorscope: saturated red is a single point in the top-left quadrant.
    const auto &vec = frame.vectorscope;
    std::uint32_t non_zero = 0;
    std::uint32_t peak_row = 0;
    std::uint32_t peak_col = 0;
    for (std::uint32_t r = 0; r < kScopeLevels; ++r) {
        for (std::uint32_t c = 0; c < kScopeLevels; ++c) {
            if (vec.density[static_cast<std::size_t>(r) * kScopeLevels + c] != 0) {
                ++non_zero;
                peak_row = r;
                peak_col = c;
            }
        }
    }
    gl_check(non_zero == 1, "readback: saturated red is one vectorscope bin");
    gl_check(peak_row == 0, "readback: red lands on the top row");
    gl_check(peak_col < kScopeLevels / 2, "readback: red lands on the left");
}

// A legitimate all-black frame must read back as real pixels all at value 0 -
// it must be distinguishable from a failed or empty readback, which publishes
// no frame (or counts a failure).
void test_readback_black(GlContext &gl)
{
    scopes_ns::Scopes scopes(gl.context);
    std::optional<ScopeFrame> frame_opt;
    const auto drain = [&scopes, &frame_opt] {
        for (int i = 0; i < 400; ++i) {
            auto candidate = scopes.latest();
            if (candidate.has_value()) {
                frame_opt = candidate;
                std::uint32_t counted = 0;
                for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
                    counted += candidate->histogram.red[level];
                }
                if (counted > 0) {
                    return; // a completed frame, whatever its colour
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    const auto log = run_readback_pipeline(gl, scopes, "black", 30, drain);
    gl_check(log != nullptr, "the black readback pipeline builds and runs");
    if (log == nullptr) {
        return;
    }

    const scopes_ns::ReadbackStats stats = scopes.readback_stats();
    gl_check(frame_opt.has_value(), "the black frame completes a readback");
    gl_check(stats.failed == 0, "the black readback passes the framebuffer check");
    if (!frame_opt.has_value()) {
        return;
    }

    const ScopeFrame &frame = *frame_opt;
    std::uint32_t pixels = 0;
    for (std::uint32_t i = 0; i < kScopeLevels; ++i) {
        pixels += frame.histogram.red[i];
    }
    gl_check(pixels > 0, "the black frame's histogram counts real pixels");
    gl_check(frame.histogram.red[0] == pixels, "black: every pixel is red 0");
    gl_check(frame.histogram.green[0] == pixels, "black: every pixel is green 0");
    gl_check(frame.histogram.blue[0] == pixels, "black: every pixel is blue 0");
}

// A WRAP_SYSMEM GstGLMemory wrapping CPU bytes: the texture is only allocated
// (empty storage) with TRANSFER_NEED_UPLOAD set, and the bytes are uploaded by
// nothing short of a GST_MAP_READ|GST_MAP_GL map. This is the exact state a
// raw-data glupload leaves a frame in when a RGBA->RGBA glcolorconvert passes
// it through unmapped - the CI black-readback case. `pixels` owns the wrapped
// bytes and must outlive the readback.
struct WrappedSysmem
{
    std::vector<std::uint8_t> pixels;
    GstBuffer *buffer = nullptr;
    guint texture_id = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    ~WrappedSysmem()
    {
        if (buffer != nullptr) {
            gst_buffer_unref(buffer);
        }
    }
};

std::unique_ptr<WrappedSysmem> make_wrapped_sysmem(GstGLContext *context, std::uint32_t width,
                                                   std::uint32_t height, std::uint8_t r,
                                                   std::uint8_t g, std::uint8_t b)
{
    auto out = std::make_unique<WrappedSysmem>();
    out->width = width;
    out->height = height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    out->pixels.assign(count * 4, 0);
    for (std::size_t i = 0; i < count; ++i) {
        out->pixels[i * 4 + 0] = r;
        out->pixels[i * 4 + 1] = g;
        out->pixels[i * 4 + 2] = b;
        out->pixels[i * 4 + 3] = 255;
    }

    GstVideoInfo v_info;
    gst_video_info_init(&v_info);
    gst_video_info_set_format(&v_info, GST_VIDEO_FORMAT_RGBA, width, height);

    gst_gl_memory_init_once();
    GstGLVideoAllocationParams *params = gst_gl_video_allocation_params_new_wrapped_data(
            context, nullptr, &v_info, 0, nullptr, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA,
            out->pixels.data(), nullptr, nullptr);
    gl_check(params != nullptr, "wrapped sysmem allocation params create");
    if (params == nullptr) {
        return nullptr;
    }

    GstGLMemoryAllocator *allocator = gst_gl_memory_allocator_get_default(context);
    gl_check(allocator != nullptr, "a GL memory allocator is available");
    if (allocator == nullptr) {
        gst_gl_allocation_params_free(reinterpret_cast<GstGLAllocationParams *>(params));
        return nullptr;
    }

    out->buffer = gst_buffer_new();
    GstGLFormat tex_formats[1] = { GST_GL_RGBA };
    gpointer wrapped_ptrs[1] = { out->pixels.data() };
    const gboolean attached = gst_gl_memory_setup_buffer(allocator, out->buffer, params,
                                                         tex_formats, wrapped_ptrs, 1);
    gl_check(attached, "wrapped sysmem memory attaches to the buffer");
    gst_object_unref(allocator);
    gst_gl_allocation_params_free(reinterpret_cast<GstGLAllocationParams *>(params));
    if (!attached) {
        return nullptr;
    }

    GstMemory *memory = gst_buffer_peek_memory(out->buffer, 0);
    if (memory != nullptr && gst_is_gl_memory(memory)) {
        out->texture_id = gst_gl_memory_get_texture_id(GST_GL_MEMORY_CAST(memory));
        // Pre-condition for the regression: the bytes are still pending upload
        // (the readback must materialise them, or they never reach the texture).
        gl_check(GST_MEMORY_FLAG_IS_SET(memory, GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD),
                 "wrapped sysmem memory is pending upload before the read");
    }
    gl_check(out->texture_id != 0, "wrapped sysmem memory has a texture");
    if (out->texture_id == 0) {
        return nullptr;
    }
    return out;
}

void test_readback_materializes_wrapped_sysmem(GlContext &gl)
{
    auto sysmem = make_wrapped_sysmem(gl.context, 16, 16, 255, 0, 0);
    gl_check(sysmem != nullptr, "a wrapped sysmem frame is built");
    if (sysmem == nullptr) {
        return;
    }

    const auto keep_alive = [&sysmem] {
        GstBuffer *ref = gst_buffer_ref(sysmem->buffer);
        return std::shared_ptr<void>(static_cast<void *>(ref), [](void *p) {
            gst_buffer_unref(static_cast<GstBuffer *>(p));
        });
    };

    scopes_ns::Scopes scopes(gl.context);
    const scopes_ns::ReadbackStats before = scopes.readback_stats();

    // Two submissions, one frame behind: the second publishes the first. The
    // worker maps the pending-upload memory before each read; without that map
    // the texture stays empty and the frame comes back black.
    scopes.read_back(sysmem->texture_id, 16, 16, keep_alive(), GL_TEXTURE_2D);
    scopes_ns::ReadbackStats stage = before;
    for (int i = 0; i < 200 && stage.processed == before.processed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stage = scopes.readback_stats();
    }
    gl_check(stage.processed == before.processed + 1,
             "materialize test: the first submission is processed");
    gl_check(stage.materialize_failed == before.materialize_failed,
             "materialize test: the pending-upload map does not fail");

    scopes.read_back(sysmem->texture_id, 16, 16, keep_alive(), GL_TEXTURE_2D);
    std::optional<ScopeFrame> frame;
    for (int i = 0; i < 200 && !frame.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame = scopes.latest();
    }
    gl_check(frame.has_value(), "materialize test: a frame publishes");
    if (frame.has_value()) {
        std::uint32_t pixels = 0;
        for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
            pixels += frame->histogram.red[level];
        }
        gl_check(pixels == 16 * 16, "materialize test: 16x16 pixels read back");
        gl_check(frame->histogram.red[255] == pixels,
                 "materialize test: the wrapped bytes upload as red");
    }
}

// A WRAP_SYSMEM frame whose OWNING context is DISTINCT from the readback
// context must still read back its uploaded bytes: the upload runs on the
// owner's context, and the readback must fence on a private sync point set on
// the owner after the upload (the producer's own sync meta can predate that
// upload, so it does not order it). The materialize test above wraps the memory
// on the reader itself, so it exercises only the same-context path.
void test_readback_distinct_context_upload(GlContext &gl)
{
    // A real producer context in the reader's share group. The reader is
    // gl.context; the owner is this producer.
    GstGLContext *producer = gst_gl_context_new(gl.display);
    GError *error = nullptr;
    if (!gst_gl_context_create(producer, gl.context, &error)) {
        gl_check(false,
                 std::string("distinct-context test: a sharing producer "
                             "context creates: ")
                         + (error != nullptr ? error->message : "?"));
        g_clear_error(&error);
        gst_object_unref(producer);
        return;
    }

    // Pre-conditions: the owner and reader are distinct contexts in one share
    // group, so the upload is genuinely cross-context and the fence is needed.
    gl_check(producer != gl.context,
             "distinct-context test: owner and reader are distinct contexts");
    gl_check(gst_gl_context_can_share(gl.context, producer),
             "distinct-context test: owner and reader share a context group");

    auto sysmem = make_wrapped_sysmem(producer, 16, 16, 255, 0, 0);
    gl_check(sysmem != nullptr, "distinct-context test: a frame is built");
    if (sysmem == nullptr) {
        gst_object_unref(producer);
        return;
    }
    // The bytes are still pending upload before the read: the readback must
    // materialise them on the owner, then fence the reader on that write.
    GstMemory *memory = gst_buffer_peek_memory(sysmem->buffer, 0);
    gl_check(memory != nullptr && gst_is_gl_memory(memory)
                     && GST_MEMORY_FLAG_IS_SET(memory, GST_GL_BASE_MEMORY_TRANSFER_NEED_UPLOAD),
             "distinct-context test: memory is pending upload before the read");

    const auto keep_alive = [&sysmem] {
        GstBuffer *ref = gst_buffer_ref(sysmem->buffer);
        return std::shared_ptr<void>(static_cast<void *>(ref), [](void *p) {
            gst_buffer_unref(static_cast<GstBuffer *>(p));
        });
    };

    scopes_ns::Scopes scopes(gl.context);
    const scopes_ns::ReadbackStats before = scopes.readback_stats();

    // Two submissions, one frame behind: the second publishes the first. Each
    // submission uploads on the owner and fences the reader before sampling.
    scopes.read_back(sysmem->texture_id, 16, 16, keep_alive(), GL_TEXTURE_2D);
    scopes_ns::ReadbackStats stage = before;
    for (int i = 0; i < 200 && stage.processed == before.processed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stage = scopes.readback_stats();
    }
    gl_check(stage.processed == before.processed + 1,
             "distinct-context test: the first submission is processed");
    gl_check(stage.materialize_failed == before.materialize_failed,
             "distinct-context test: the cross-context upload does not fail");
    gl_check(stage.materialize_synced == before.materialize_synced + 1,
             "distinct-context test: the owner->reader fence is established");

    scopes.read_back(sysmem->texture_id, 16, 16, keep_alive(), GL_TEXTURE_2D);
    std::optional<ScopeFrame> frame;
    for (int i = 0; i < 200 && !frame.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame = scopes.latest();
    }
    gl_check(frame.has_value(), "distinct-context test: a frame publishes");
    if (frame.has_value()) {
        std::uint32_t pixels = 0;
        for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
            pixels += frame->histogram.red[level];
        }
        gl_check(pixels == 16 * 16, "distinct-context test: 16x16 pixels read back");
        gl_check(frame->histogram.red[255] == pixels,
                 "distinct-context test: the owner's upload reads back as red");
    }

    gst_object_unref(producer);
}

// A GL texture of one solid colour, created on the GL thread for the PBO resize
// regression (the readback needs real textures of different sizes).
struct SolidTexture
{
    GLuint name = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

void create_solid_texture_on_gl(GstGLContext *context, gpointer user)
{
    auto *tex = static_cast<SolidTexture *>(user);
    const GstGLFuncs *gl = context->gl_vtable;
    if (gl->GenTextures == nullptr || gl->BindTexture == nullptr || gl->TexImage2D == nullptr) {
        return;
    }
    const std::size_t count = static_cast<std::size_t>(tex->width) * tex->height;
    std::vector<std::uint8_t> pixels(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        pixels[i * 4 + 0] = tex->r;
        pixels[i * 4 + 1] = tex->g;
        pixels[i * 4 + 2] = tex->b;
        pixels[i * 4 + 3] = 255;
    }
    gl->GenTextures(1, &tex->name);
    gl->BindTexture(GL_TEXTURE_2D, tex->name);
    gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(tex->width),
                   static_cast<GLsizei>(tex->height), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    gl->BindTexture(GL_TEXTURE_2D, 0);
}

void delete_solid_texture_on_gl(GstGLContext *context, gpointer user)
{
    auto *tex = static_cast<SolidTexture *>(user);
    const GstGLFuncs *gl = context->gl_vtable;
    if (tex->name != 0 && gl->DeleteTextures != nullptr) {
        gl->DeleteTextures(1, &tex->name);
        tex->name = 0;
    }
}

// The two PBO slots are sized independently: when a frame changes size, only
// the slot about to be written is reallocated - the other slot still holds the
// previous frame's readback at its old size, and touching it would clobber the
// frame it is about to hand back.
void test_readback_pbo_resize(GlContext &gl)
{
    SolidTexture red{ 0, 32, 32, 255, 0, 0 };
    SolidTexture green{ 0, 8, 8, 0, 255, 0 };
    SolidTexture blue{ 0, 16, 16, 0, 0, 255 };
    gst_gl_context_thread_add(gl.context, create_solid_texture_on_gl, &red);
    gst_gl_context_thread_add(gl.context, create_solid_texture_on_gl, &green);
    gst_gl_context_thread_add(gl.context, create_solid_texture_on_gl, &blue);
    gl_check(red.name != 0 && green.name != 0 && blue.name != 0,
             "resize test: three textures create");
    if (red.name == 0 || green.name == 0 || blue.name == 0) {
        return;
    }

    scopes_ns::Scopes scopes(gl.context);
    const scopes_ns::ReadbackStats before = scopes.readback_stats();

    // Red 32x32, then green 8x8: the shrink must not clobber the red frame
    // still pending in the other PBO slot.
    scopes.read_back(red.name, 32, 32, { }, GL_TEXTURE_2D);
    scopes_ns::ReadbackStats stage = before;
    for (int i = 0; i < 200 && stage.processed == before.processed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stage = scopes.readback_stats();
    }
    gl_check(stage.processed == before.processed + 1,
             "resize test: the red submission is processed");

    scopes.read_back(green.name, 8, 8, { }, GL_TEXTURE_2D);
    for (int i = 0; i < 200 && stage.processed < before.processed + 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stage = scopes.readback_stats();
    }
    gl_check(stage.processed >= before.processed + 2,
             "resize test: the green submission is processed");

    std::optional<ScopeFrame> frame;
    for (int i = 0; i < 200 && !frame.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame = scopes.latest();
    }
    gl_check(frame.has_value(), "resize test: the red frame publishes");
    if (frame.has_value()) {
        std::uint32_t pixels = 0;
        for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
            pixels += frame->histogram.red[level];
        }
        gl_check(pixels == 32 * 32, "resize test: the red frame is 32x32");
        gl_check(frame->histogram.red[255] == pixels,
                 "resize test: the red frame is saturated red");
    }

    // Blue 16x16 grows back up; it publishes the green 8x8 frame.
    scopes.read_back(blue.name, 16, 16, { }, GL_TEXTURE_2D);
    for (int i = 0; i < 200 && stage.processed < before.processed + 3; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stage = scopes.readback_stats();
    }
    gl_check(stage.processed >= before.processed + 3,
             "resize test: the blue submission is processed");

    frame.reset();
    for (int i = 0; i < 200 && !frame.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame = scopes.latest();
    }
    gl_check(frame.has_value(), "resize test: the green frame publishes");
    if (frame.has_value()) {
        std::uint32_t pixels = 0;
        for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
            pixels += frame->histogram.green[level];
        }
        gl_check(pixels == 8 * 8, "resize test: the green frame is 8x8");
        gl_check(frame->histogram.green[255] == pixels,
                 "resize test: the green frame is saturated green");
    }

    gst_gl_context_thread_add(gl.context, delete_solid_texture_on_gl, &red);
    gst_gl_context_thread_add(gl.context, delete_solid_texture_on_gl, &green);
    gst_gl_context_thread_add(gl.context, delete_solid_texture_on_gl, &blue);
}

// The readback must reject a texture attached with an unsupported GL target
// (glFramebufferTexture2D only accepts a 2D-family target) instead of reading
// garbage or crashing, and must still read the same texture when attached
// correctly.
struct TextureName
{
    GLuint name = 0;
};

void create_red_2d_texture_on_gl(GstGLContext *context, gpointer user)
{
    auto *created = static_cast<TextureName *>(user);
    const GstGLFuncs *gl = context->gl_vtable;
    if (gl->GenTextures == nullptr || gl->BindTexture == nullptr || gl->TexImage2D == nullptr) {
        return;
    }
    const std::uint8_t pixel[4] = { 255, 0, 0, 255 };
    gl->GenTextures(1, &created->name);
    gl->BindTexture(GL_TEXTURE_2D, created->name);
    gl->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    gl->BindTexture(GL_TEXTURE_2D, 0);
}

void delete_texture_on_gl(GstGLContext *context, gpointer user)
{
    auto *created = static_cast<TextureName *>(user);
    const GstGLFuncs *gl = context->gl_vtable;
    if (created->name != 0 && gl->DeleteTextures != nullptr) {
        gl->DeleteTextures(1, &created->name);
        created->name = 0;
    }
}

void test_readback_rejects_bad_target(GlContext &gl)
{
    TextureName texture;
    gst_gl_context_thread_add(gl.context, create_red_2d_texture_on_gl, &texture);
    gl_check(texture.name != 0, "a 2D source texture is created for the target test");
    if (texture.name == 0) {
        return;
    }

    scopes_ns::Scopes scopes(gl.context);
    const scopes_ns::ReadbackStats before = scopes.readback_stats();

    // GL_TEXTURE_3D is not a valid glFramebufferTexture2D target: the FBO stays
    // incomplete and nothing is read.
    scopes.read_back(texture.name, 1, 1, { }, GL_TEXTURE_3D);
    scopes_ns::ReadbackStats after = before;
    for (int i = 0; i < 100 && after.failed == before.failed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        after = scopes.readback_stats();
    }
    gl_check(after.failed > before.failed,
             "an unsupported texture target increments the failed count");
    gl_check(after.completed == before.completed,
             "an unsupported texture target publishes no frame");
    gl_check(!after.last_fbo_complete,
             "an unsupported texture target leaves the framebuffer incomplete");
    gl_check(after.last_target == GL_TEXTURE_3D,
             "the failed attempt records the bad target it used");

    // Positive control: the same texture attached as GL_TEXTURE_2D reads back.
    // The readback is one frame behind (a submission completes the previous
    // frame's PBO), so a single submission publishes nothing: submit two, each
    // confirmed processed, so the second publishes the first.
    const scopes_ns::ReadbackStats control_before = scopes.readback_stats();
    scopes.read_back(texture.name, 1, 1, { }, GL_TEXTURE_2D);
    scopes_ns::ReadbackStats first = control_before;
    for (int i = 0; i < 200 && first.processed == control_before.processed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        first = scopes.readback_stats();
    }
    gl_check(first.processed == control_before.processed + 1,
             "the first 2D control submission is processed");
    gl_check(first.failed == control_before.failed, "the 2D control submission does not fail");

    scopes.read_back(texture.name, 1, 1, { }, GL_TEXTURE_2D);
    std::optional<ScopeFrame> frame;
    for (int i = 0; i < 200 && !frame.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame = scopes.latest();
    }
    gl_check(frame.has_value(), "the 2D control reads back a frame");
    if (frame.has_value()) {
        std::uint32_t pixels = 0;
        for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
            pixels += frame->histogram.red[level];
        }
        gl_check(pixels == 1, "the 1x1 red control is exactly one pixel");
        gl_check(frame->histogram.red[255] == 1, "the control pixel is saturated red");
    }

    gst_gl_context_thread_add(gl.context, delete_texture_on_gl, &texture);
}

// The readback must wait on a producer's GstGLSyncMeta fence before sampling a
// texture the producer painted on a sharing context - otherwise it can capture
// the texture before it is written. Create a producer context sharing the
// readback context (as GStreamer does for an app-supplied context), paint the
// texture on it, signal a sync point on it, and read it back on the readback
// context.
void test_readback_sync_meta(GlContext &gl)
{
    GstGLContext *producer = gst_gl_context_new(gl.display);
    GError *error = nullptr;
    if (!gst_gl_context_create(producer, gl.context, &error)) {
        gl_check(false,
                 std::string("a sharing producer context creates: ")
                         + (error != nullptr ? error->message : "?"));
        g_clear_error(&error);
        gst_object_unref(producer);
        return;
    }

    // Paint the texture on the producer context, so the readback on gl.context
    // is genuinely cross-context.
    TextureName texture;
    gst_gl_context_thread_add(producer, create_red_2d_texture_on_gl, &texture);
    gl_check(texture.name != 0, "sync test: the producer paints a texture");
    if (texture.name == 0) {
        gst_object_unref(producer);
        return;
    }

    // A buffer carrying the producer's sync meta, as a GL element attaches one
    // to its output; the fence is signalled on the producer context. Allocate
    // a byte so buffer_gl_info's memory peek has a memory to look at.
    GstBuffer *buffer = gst_buffer_new_allocate(nullptr, 4, nullptr);
    GstGLSyncMeta *meta = gst_buffer_add_gl_sync_meta(producer, buffer);
    gl_check(meta != nullptr, "sync test: a sync meta attaches to the buffer");
    if (meta != nullptr) {
        gst_gl_sync_meta_set_sync_point(meta, producer);
    }

    const auto keep_alive = [](GstBuffer *b) {
        GstBuffer *ref = gst_buffer_ref(b);
        return std::shared_ptr<void>(static_cast<void *>(ref), [](void *p) {
            gst_buffer_unref(static_cast<GstBuffer *>(p));
        });
    };

    scopes_ns::Scopes scopes(gl.context);
    const scopes_ns::ReadbackStats before = scopes.readback_stats();

    // Two submissions, one frame behind: the second publishes the first.
    scopes.read_back(texture.name, 1, 1, keep_alive(buffer), GL_TEXTURE_2D);
    scopes_ns::ReadbackStats stage = before;
    for (int i = 0; i < 200 && stage.processed == before.processed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        stage = scopes.readback_stats();
    }
    gl_check(stage.processed > before.processed, "sync test: the submission is processed");
    gl_check(stage.last_sync_meta, "sync test: the producer's sync meta is found");
    gl_check(stage.sync_context_shared, "sync test: the producer context is shared");
    gl_check(stage.failed == before.failed, "sync test: the shared readback does not fail");

    scopes.read_back(texture.name, 1, 1, keep_alive(buffer), GL_TEXTURE_2D);
    std::optional<ScopeFrame> frame;
    for (int i = 0; i < 200 && !frame.has_value(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        frame = scopes.latest();
    }
    gl_check(frame.has_value(), "sync test: the shared readback publishes a frame");
    if (frame.has_value()) {
        std::uint32_t pixels = 0;
        for (std::uint32_t level = 0; level < kScopeLevels; ++level) {
            pixels += frame->histogram.red[level];
        }
        gl_check(pixels == 1, "sync test: the readback is exactly one pixel");
        gl_check(frame->histogram.red[255] == 1, "sync test: the pixel is saturated red");
    }

    gst_buffer_unref(buffer);
    gst_gl_context_thread_add(gl.context, delete_texture_on_gl, &texture);
    gst_object_unref(producer);
}

// The readback borrows GL state (pack alignment/row length, PBO and framebuffer
// bindings) and must leave it exactly as found, because the app renders on the
// same context between readbacks.
struct PackState
{
    GLint alignment = -1;
    GLint row_length = -1;
    GLint pack_buffer = -1;
    GLint framebuffer = -1;
    GLint read_framebuffer = -1;
    GLint draw_framebuffer = -1;
    bool separate_read_draw = false;
};

void capture_pack_state_on_gl(GstGLContext *context, gpointer user)
{
    auto *state = static_cast<PackState *>(user);
    const GstGLFuncs *gl = context->gl_vtable;
    if (gl->GetIntegerv == nullptr) {
        return;
    }
    gl->GetIntegerv(GL_PACK_ALIGNMENT, &state->alignment);
    gl->GetIntegerv(GL_PACK_ROW_LENGTH, &state->row_length);
    gl->GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &state->pack_buffer);
    // GLES2/desktop GL < 3.0 have a single framebuffer binding; GLES3/desktop
    // GL >= 3.0 split it into read and draw. Capture whichever pair the context
    // exposes so the assertion matches what the readback restores.
    state->separate_read_draw = gst_gl_context_check_gl_version(
            context,
            static_cast<GstGLAPI>(GST_GL_API_OPENGL | GST_GL_API_OPENGL3 | GST_GL_API_GLES2), 3, 0);
    if (state->separate_read_draw) {
        gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->read_framebuffer);
        gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_framebuffer);
    } else {
        gl->GetIntegerv(GL_FRAMEBUFFER_BINDING, &state->framebuffer);
    }
}

void test_readback_restores_gl_state(GlContext &gl)
{
    PackState before;
    gst_gl_context_thread_add(gl.context, capture_pack_state_on_gl, &before);

    TextureName texture;
    gst_gl_context_thread_add(gl.context, create_red_2d_texture_on_gl, &texture);
    gl_check(texture.name != 0, "state test: a texture creates");
    if (texture.name == 0) {
        return;
    }

    {
        scopes_ns::Scopes scopes(gl.context);
        // Two readbacks, each confirmed processed: the first sets up the PBO
        // and fence, the second runs the "complete previous" path, so the whole
        // state-borrowing slice is exercised before the state is captured.
        const scopes_ns::ReadbackStats before = scopes.readback_stats();
        scopes.read_back(texture.name, 1, 1, { }, GL_TEXTURE_2D);
        scopes_ns::ReadbackStats stage = before;
        for (int i = 0; i < 200 && stage.processed == before.processed; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            stage = scopes.readback_stats();
        }
        scopes.read_back(texture.name, 1, 1, { }, GL_TEXTURE_2D);
        for (int i = 0; i < 200 && stage.processed < before.processed + 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            stage = scopes.readback_stats();
        }
        gl_check(stage.processed >= before.processed + 2, "state test: both readbacks run");
    }

    PackState after;
    gst_gl_context_thread_add(gl.context, capture_pack_state_on_gl, &after);
    gl_check(after.alignment == before.alignment, "state test: pack alignment is restored");
    gl_check(after.row_length == before.row_length, "state test: pack row length is restored");
    gl_check(after.pack_buffer == before.pack_buffer,
             "state test: pack buffer binding is restored");
    gl_check(after.framebuffer == before.framebuffer,
             "state test: framebuffer binding is restored");
    if (after.separate_read_draw) {
        gl_check(after.read_framebuffer == before.read_framebuffer,
                 "state test: read framebuffer binding is restored");
        gl_check(after.draw_framebuffer == before.draw_framebuffer,
                 "state test: draw framebuffer binding is restored");
    }

    gst_gl_context_thread_add(gl.context, delete_texture_on_gl, &texture);
}

// Destroying a Scopes while a readback is in flight must not corrupt the
// context: ~Scopes joins the worker, frees its GL resources on the GL thread,
// and drops the engine buffers while their contexts are still alive. Run a few
// rounds, each leaving a frame in flight, and prove the context still runs GL
// work afterwards.
void test_async_teardown(GlContext &gl)
{
    for (int round = 0; round < 4; ++round) {
        TextureName texture;
        gst_gl_context_thread_add(gl.context, create_red_2d_texture_on_gl, &texture);
        gl_check(texture.name != 0, "async teardown: a texture creates");
        if (texture.name == 0) {
            return;
        }

        {
            scopes_ns::Scopes scopes(gl.context);
            // Two submissions keep a frame in flight when the Scopes is
            // destroyed: the first is one frame behind and the second's
            // readback may not have completed.
            scopes.read_back(texture.name, 1, 1, { }, GL_TEXTURE_2D);
            scopes.read_back(texture.name, 1, 1, { }, GL_TEXTURE_2D);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } // Scopes destroyed here, possibly mid-readback.

        // The context must still run GL work.
        TextureName probe;
        gst_gl_context_thread_add(gl.context, create_red_2d_texture_on_gl, &probe);
        gl_check(probe.name != 0, "async teardown: the context still creates a texture");
        if (probe.name != 0) {
            gst_gl_context_thread_add(gl.context, delete_texture_on_gl, &probe);
        }
        gst_gl_context_thread_add(gl.context, delete_texture_on_gl, &texture);
    }
}

// Measures the frame rate over a fixed wall-clock interval, with and without
// the readback in the frame path. Returns the frames-per-second.
double measure_fps(GlContext &gl, bool with_scopes)
{
    constexpr int kSeconds = 2;

    GstElement *pipeline = gst_pipeline_new("measure");
    GstElement *src = gst_element_factory_make("videotestsrc", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *upload = gst_element_factory_make("glupload", nullptr);
    GstElement *colorconvert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", nullptr);
    if (pipeline == nullptr || src == nullptr || convert == nullptr || upload == nullptr
        || colorconvert == nullptr || filter == nullptr || appsink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&src);
        gst_clear_object(&convert);
        gst_clear_object(&upload);
        gst_clear_object(&colorconvert);
        gst_clear_object(&filter);
        gst_clear_object(&appsink);
        return -1.0;
    }

    gst_util_set_object_arg(G_OBJECT(src), "pattern", "smpte");
    g_object_set(src, "num-buffers", -1, nullptr);
    GstCaps *caps =
            gst_caps_from_string("video/"
                                 "x-raw(memory:GLMemory),format=(string)RGBA,width=320,height=240");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(appsink, "sync", FALSE, "max-buffers", 1, "drop", TRUE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), src, convert, upload, colorconvert, filter, appsink,
                     nullptr);
    if (!gst_element_link_many(src, convert, upload, colorconvert, filter, appsink, nullptr)) {
        gst_object_unref(pipeline);
        return -1.0;
    }

    std::atomic<int> frames{ 0 };
    scopes_ns::Scopes *scopes_ptr = nullptr;
    scopes_ns::Scopes scopes{ gl.context };
    if (with_scopes) {
        scopes_ptr = &scopes;
    }

    // The callback counts every frame; with scopes it also drives read_back.
    struct MeasureCtx
    {
        std::atomic<int> *frames;
        scopes_ns::Scopes *scopes;
    } ctx{ &frames, scopes_ptr };

    auto on_sample = [](GstAppSink *sink, gpointer user_data) -> GstFlowReturn {
        auto *mctx = static_cast<MeasureCtx *>(user_data);
        GstSample *sample = gst_app_sink_pull_sample(sink);
        if (sample == nullptr) {
            return GST_FLOW_ERROR;
        }
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstCaps *scaps = gst_sample_get_caps(sample);
        if (mctx->scopes != nullptr && buffer != nullptr && scaps != nullptr) {
            GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
            if (memory != nullptr && gst_is_gl_memory(memory)) {
                const guint texture_id = gst_gl_memory_get_texture_id(GST_GL_MEMORY_CAST(memory));
                const GstStructure *structure = gst_caps_get_structure(scaps, 0);
                gint width = 0;
                gint height = 0;
                gst_structure_get_int(structure, "width", &width);
                gst_structure_get_int(structure, "height", &height);
                GstBuffer *keep = gst_buffer_ref(buffer);
                std::shared_ptr<void> keep_alive(static_cast<void *>(keep), [](void *p) {
                    gst_buffer_unref(static_cast<GstBuffer *>(p));
                });
                mctx->scopes->read_back(texture_id, width, height, std::move(keep_alive));
            }
        }
        mctx->frames->fetch_add(1);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    };

    GstAppSinkCallbacks callbacks = { };
    callbacks.new_sample = on_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, &ctx, nullptr);

    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_set_sync_handler(bus, on_bus_sync, &gl, nullptr);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    std::this_thread::sleep_for(std::chrono::seconds(kSeconds));
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return static_cast<double>(frames.load()) / kSeconds;
}

void test_frame_rate_not_stalled(GlContext &gl)
{
    const double baseline = measure_fps(gl, false);
    const double scoped = measure_fps(gl, true);
    std::printf("fps: baseline=%.1f with-scopes=%.1f\n", baseline, scoped);
    gl_check(baseline > 0.0, "the baseline pipeline measures a positive rate");
    gl_check(scoped > 0.0, "the scoped pipeline measures a positive rate");
    // The non-blocking readback must not stall the render path: the rate stays
    // within half of the baseline (a synchronous glReadPixels collapses it to
    // a crawl, which this catches).
    gl_check(scoped >= baseline * 0.5,
             "the readback does not stall the pipeline (>= half baseline)");
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

    test_read_scopes_nullopt_before_readback(gl);
    test_readback_solid_red(gl);
    test_readback_black(gl);
    test_readback_materializes_wrapped_sysmem(gl);
    test_readback_distinct_context_upload(gl);
    test_readback_pbo_resize(gl);
    test_readback_rejects_bad_target(gl);
    test_readback_sync_meta(gl);
    test_readback_restores_gl_state(gl);
    test_async_teardown(gl);
    test_frame_rate_not_stalled(gl);
}

} // namespace

int main()
{
    // Pure tests: deterministic scope reduction, no GStreamer or GL needed.
    test_solid_red_histogram_peaks();
    test_gradient_spreads_not_spikes();
    test_waveform_flat_on_solid();
    test_vectorscope_red_lands_top_left();
    std::printf("pure: %d checks, %d failures\n", checks, failures);

    // GL-backed tests: drive a real pipeline headlessly and read it back.
    run_gl_tests();
    std::printf("gl: %d checks, %d failures\n", gl_checks, gl_failures);

    const int total = failures + gl_failures;
    std::printf("%d checks, %d failures\n", checks + gl_checks, total);
    return total == 0 ? 0 : 1;
}
