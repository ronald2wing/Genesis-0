// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/session/GesSession.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <ges/ges.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <gst/gl/gl.h>

#include "adapters/engine/ges/media/Colorimetry.h"
#include "adapters/engine/ges/session/GesBuilder.h"
#include "adapters/engine/ges/export/DecodePolicy.h"
#include "adapters/engine/ges/export/Hardware.h"
#include "render/Resolve.h"

namespace genesis::adapters::engine::ges {

// The GStreamer-side presentation state the GL upload chain needs. It owns the
// wrapped EGL display and context (the app's raw handles, wrapped so GStreamer
// can share them) and the frame sink the appsink hands frames to. The
// callbacks below receive this as their user_data; it outlives the pipeline
// because the session tears the pipeline down before dropping it.
struct GesSession::Presentation
{
    GstGLDisplay *display = nullptr;
    GstGLContext *context = nullptr;
    genesis::adapters::engine::FrameSink sink;

    // The appsink the callback is attached to, held so teardown can detach the
    // callback before the pipeline is dropped. A ref is kept for the sink's
    // lifetime; the pipeline owns the bin, this is a second ref.
    GstElement *appsink = nullptr;

    // Set before the pipeline is torn down. The appsink's new_sample runs on a
    // GStreamer streaming thread that a wedged decoder can keep alive past the
    // state-change wait; once this is set the callback returns without touching
    // the sink, so a late frame cannot call into a half-destroyed host.
    std::atomic<bool> detached{ false };

    // The presentation timestamp of the last frame handed to the sink, and the
    // floor a seek imposes. A flush seek re-prerolls, and the appsink can
    // deliver a frame from the old position after the target frame; presenting
    // it would flash the picture backward. Frames older than the floor are
    // dropped, and the floor advances with each delivered frame so delivery is
    // monotonic. GST_CLOCK_TIME_NONE means "no floor yet".
    std::atomic<GstClockTime> pts_floor{ GST_CLOCK_TIME_NONE };

    ~Presentation()
    {
        if (appsink != nullptr) {
            gst_object_unref(appsink);
        }
        if (context != nullptr) {
            gst_object_unref(context);
        }
        if (display != nullptr) {
            gst_object_unref(display);
        }
    }
};

namespace {

using genesis::adapters::engine::DecodePreference;
using genesis::adapters::engine::FrameTexture;
using genesis::core::Rational;

// The seam's client-API name -> the GStreamer GLAPI bit the wrapped context is
// created with. Unknown (the host did not detect one) keeps the GLES2 default,
// which is right for the headless GLES context and wrong for a desktop-GL one.
GstGLAPI gst_gl_api_from(genesis::adapters::engine::GlApi api)
{
    switch (api) {
    case genesis::adapters::engine::GlApi::OpenGL:
        return GST_GL_API_OPENGL;
    case genesis::adapters::engine::GlApi::OpenGL3:
        return GST_GL_API_OPENGL3;
    case genesis::adapters::engine::GlApi::GLES2:
    case genesis::adapters::engine::GlApi::Unknown:
        break;
    }
    return GST_GL_API_GLES2;
}

// The per-platform decode policy (DecodePolicy.cpp) owns the decoder-ranking
// tilt; the session only forwards its preference to it before load(). Ranks are
// process-global registry state, so applying the policy retilts them for the
// next pipeline and the policy restores them when the preference changes
// (including to Auto): a session never leaves the registry different from how
// it found it.

// The exact rational -> GES nanosecond seam, matching to_ns in GesBuilder.cpp:
// as_double() is the documented lossy engine-argument edge, never converted
// back. A non-finite or overflowing value clamps to the valid GstClockTime
// range rather than landing on G_MININT64 through an undefined llround.
GstClockTime to_ns(Rational time)
{
    const double ns = time.as_double() * GST_SECOND;
    if (!std::isfinite(ns) || ns <= 0.0) {
        return 0;
    }
    if (ns >= static_cast<double>(G_MAXINT64)) {
        return static_cast<GstClockTime>(G_MAXINT64);
    }
    return static_cast<GstClockTime>(std::llround(ns));
}

// The reverse seam: nanoseconds are already an exact integer, so the rational
// is exact and only reduces to lowest terms.
Rational from_ns(GstClockTime ns)
{
    return Rational(static_cast<std::int64_t>(ns), static_cast<std::int64_t>(GST_SECOND));
}

// Drives the pipeline to `state` and blocks until it settles, or fails. GES
// state changes complete asynchronously, but a blocking get_state is enough
// without a GMainLoop: the pipeline advances its own state from the streaming
// thread. A timeout (ASYNC after the budget) is reported distinctly from a
// hard failure so the caller can log which one happened.
bool block_until_state(GstElement *element, GstState state)
{
    if (gst_element_set_state(element, state) == GST_STATE_CHANGE_FAILURE) {
        return false;
    }
    GstState current = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn get =
            gst_element_get_state(element, &current, nullptr, 5 * GST_SECOND);
    return get != GST_STATE_CHANGE_FAILURE && current == state;
}

// The same wait, but reports how it ended so a caller can name a timeout apart
// from a hard failure in the log. `out_current` receives the state the element
// settled in (or VOID_PENDING when it never left the transition).
bool block_until_state_verbose(GstElement *element, GstState state, GstState *out_current,
                               GstStateChangeReturn *out_result)
{
    const GstStateChangeReturn set = gst_element_set_state(element, state);
    if (set == GST_STATE_CHANGE_FAILURE) {
        *out_current = GST_STATE_VOID_PENDING;
        *out_result = set;
        return false;
    }
    GstState current = GST_STATE_VOID_PENDING;
    const GstStateChangeReturn get =
            gst_element_get_state(element, &current, nullptr, 5 * GST_SECOND);
    *out_current = current;
    *out_result = get;
    return get != GST_STATE_CHANGE_FAILURE && current == state;
}

// Drains the pipeline's bus for the diagnostic messages a stalled preroll
// leaves behind and returns them, each named and newline-separated, or an
// empty string when none are pending. A preroll that posts no ERROR (a
// decoder-less stream only posts a WARNING with a missing-plugin detail) is the
// READY-stall case this names: the warning carries the exact codec/decoder
// that is absent, so a "reload failed" is diagnosable from the log without a
// debugger.
std::string drain_bus(GstElement *element)
{
    GstBus *bus = gst_element_get_bus(element);
    if (bus == nullptr) {
        return { };
    }
    std::string out;
    for (GstMessage *msg = gst_bus_pop(bus); msg != nullptr; msg = gst_bus_pop(bus)) {
        const GstMessageType type = GST_MESSAGE_TYPE(msg);
        GError *error = nullptr;
        gchar *debug = nullptr;
        const char *label = nullptr;
        if (type == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &error, &debug);
            label = "error";
        } else if (type == GST_MESSAGE_WARNING) {
            gst_message_parse_warning(msg, &error, &debug);
            label = "warning";
        } else if (type == GST_MESSAGE_INFO) {
            gst_message_parse_info(msg, &error, &debug);
            label = "info";
        }
        if (label != nullptr) {
            out += '[';
            out += label;
            out += "] ";
            out += error != nullptr && error->message != nullptr ? error->message : "unknown";
            if (type == GST_MESSAGE_WARNING) {
                const GstStructure *structure = gst_message_get_structure(msg);
                if (structure != nullptr) {
                    const gchar *detail = gst_structure_get_string(structure, "detail");
                    if (detail != nullptr) {
                        out += " (missing-plugin: ";
                        out += detail;
                        out += ')';
                    }
                }
            }
            out += '\n';
        }
        g_clear_error(&error);
        g_free(debug);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    return out;
}

// Answers the NEED_CONTEXT messages the glupload/glcolorconvert elements post
// on the bus before negotiation, with the app's wrapped display and context.
// The sync handler is the only point early enough for the negotiation to see
// the context (the spike's finding); anything else passes through untouched.
GstBusSyncReply on_bus_sync(GstBus *bus, GstMessage *message, gpointer user_data)
{
    (void)bus;
    if (GST_MESSAGE_TYPE(message) != GST_MESSAGE_NEED_CONTEXT) {
        return GST_BUS_PASS;
    }
    auto *presentation = static_cast<GesSession::Presentation *>(user_data);

    const gchar *context_type = nullptr;
    gst_message_parse_context_type(message, &context_type);

    if (g_strcmp0(context_type, GST_GL_DISPLAY_CONTEXT_TYPE) == 0) {
        GstContext *context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
        gst_context_set_gl_display(context, presentation->display);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context);
        gst_context_unref(context);
        return GST_BUS_DROP;
    }
    if (g_strcmp0(context_type, "gst.gl.app_context") == 0) {
        GstContext *context = gst_context_new("gst.gl.app_context", TRUE);
        GstStructure *structure = gst_context_writable_structure(context);
        gst_structure_set(structure, "context", GST_TYPE_GL_CONTEXT, presentation->context,
                          nullptr);
        gst_element_set_context(GST_ELEMENT(GST_MESSAGE_SRC(message)), context);
        gst_context_unref(context);
        return GST_BUS_DROP;
    }
    return GST_BUS_PASS;
}

// Builds the engine-neutral frame from the sample and hands it to the sink.
// The sample's buffer is ref'd into the keep_alive token, so the GL texture it
// owns stays valid exactly as long as the caller holds the FrameTexture; the
// sample itself is always unreffed here.
void deliver_sample(GesSession::Presentation *presentation, GstSample *sample)
{
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (caps == nullptr || buffer == nullptr) {
        return;
    }

    GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
    if (memory == nullptr || !gst_is_gl_memory(memory)) {
        return;
    }

    // Drop a frame that would move the picture backward. A flush seek can
    // deliver a frame from the old position after the target frame; presenting
    // it flashes the monitor back. The floor is the last delivered PTS (or the
    // seek target), so delivery is monotonic and self-heals: the first frame at
    // or after the floor is delivered and becomes the new floor.
    const GstClockTime pts = GST_BUFFER_PTS(buffer);
    if (GST_CLOCK_TIME_IS_VALID(pts)) {
        GstClockTime floor = presentation->pts_floor.load(std::memory_order_relaxed);
        if (GST_CLOCK_TIME_IS_VALID(floor) && pts < floor) {
            return;
        }
        presentation->pts_floor.store(pts, std::memory_order_relaxed);
    }

    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    gint width = 0;
    gint height = 0;
    gst_structure_get_int(structure, "width", &width);
    gst_structure_get_int(structure, "height", &height);

    auto *gl_memory = GST_GL_MEMORY_CAST(memory);
    const guint texture_id = gst_gl_memory_get_texture_id(gl_memory);

    GstBuffer *keep = gst_buffer_ref(buffer);
    std::shared_ptr<void> keep_alive(static_cast<void *>(keep), [](void *p) {
        gst_buffer_unref(static_cast<GstBuffer *>(p));
    });

    FrameTexture frame;
    frame.texture_id = texture_id;
    frame.width = static_cast<std::uint32_t>(width);
    frame.height = static_cast<std::uint32_t>(height);
    frame.signal = signal_from_caps(caps);
    frame.keep_alive = std::move(keep_alive);
    presentation->sink(std::move(frame));
}

// The appsink new_sample: pull the frame, present it when a sink is set, and
// drain it otherwise. Either way the sample is released here - the keep_alive
// token is the only thing that may outlive this call.
GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data)
{
    auto *presentation = static_cast<GesSession::Presentation *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (sample == nullptr) {
        return GST_FLOW_ERROR;
    }
    // A frame can arrive after teardown began (a wedged decoder keeps the
    // streaming thread alive past the state-change wait). The sink and the host
    // it calls into are being destroyed, so drop the frame instead of
    // dereferencing them.
    if (!presentation->detached.load(std::memory_order_acquire) && presentation->sink) {
        deliver_sample(presentation, sample);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// The spike's proven upload chain, as a bin GES can attach as the preview
// video sink: glupload -> glcolorconvert -> capsfilter forcing a single RGBA
// 2D GL texture -> appsink. Returns a floating-ref bin on success, nullptr on
// failure.
GstElement *build_present_sink(GesSession::Presentation *presentation)
{
    GstElement *bin = gst_bin_new("video-present");
    GstElement *upload = gst_element_factory_make("glupload", "glupload");
    GstElement *convert = gst_element_factory_make("glcolorconvert", nullptr);
    GstElement *filter = gst_element_factory_make("capsfilter", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", "sink");
    if (bin == nullptr || upload == nullptr || convert == nullptr || filter == nullptr
        || appsink == nullptr) {
        gst_clear_object(&bin);
        gst_clear_object(&upload);
        gst_clear_object(&convert);
        gst_clear_object(&filter);
        gst_clear_object(&appsink);
        return nullptr;
    }

    // A monitor is clocked, not a drain: sync=TRUE paces the frame to the
    // pipeline clock so playback runs in real time. The spike used sync=FALSE
    // because it measured the frame path, not playback - with that setting a
    // two-second timeline dumps every frame in a burst and the position
    // readout runs away from the picture. Hold two, drop rather than block.
    g_object_set(appsink, "sync", TRUE, "max-buffers", 2, "drop", TRUE, nullptr);

    GstCaps *caps = gst_caps_from_string("video/x-raw(memory:GLMemory),format=(string)RGBA");
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    gst_bin_add_many(GST_BIN(bin), upload, convert, filter, appsink, nullptr);
    if (!gst_element_link_many(upload, convert, filter, appsink, nullptr)) {
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad *pad = gst_element_get_static_pad(upload, "sink");
    GstPad *ghost = gst_ghost_pad_new("sink", pad);
    gst_object_unref(pad);
    gst_element_add_pad(bin, ghost);

    GstAppSinkCallbacks callbacks = { };
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, presentation, nullptr);

    // Hold a ref so teardown can detach the callback before the pipeline (and
    // the bin that owns the appsink) is dropped.
    presentation->appsink = GST_ELEMENT(gst_object_ref(appsink));

    return bin;
}

// A clocked drain: sync=TRUE keeps the audio playhead on the pipeline clock so
// position() advances in real time even with no device attached. Returns
// nullptr only if the factory is missing (nothing else can fail here).
GstElement *make_audio_fakesink()
{
    GstElement *sink = gst_element_factory_make("fakesink", nullptr);
    if (sink != nullptr) {
        g_object_set(sink, "sync", TRUE, nullptr);
    }
    return sink;
}

// Whether `sink` can actually reach a device. Driving a bare sink to PAUSED
// connects a real audio sink to its backend but never fully prerolls (there is
// no upstream clock or data), so a hard state-change failure is the no-device
// signal for the explicit backends. autoaudiosink instead silently keeps its
// internal "fake-audio-sink" child when no backend is usable, so it counts as
// opened only when the READY->PAUSED transition swapped that child for a real
// one (the swap is synchronous within set_state).
bool audio_sink_opens(GstElement *sink)
{
    const GstStateChangeReturn ret = gst_element_set_state(sink, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        gst_element_set_state(sink, GST_STATE_NULL);
        return false;
    }
    if (GST_IS_BIN(sink)) {
        GstElement *fake = gst_bin_get_by_name(GST_BIN(sink), "fake-audio-sink");
        if (fake != nullptr) {
            gst_object_unref(fake);
            gst_element_set_state(sink, GST_STATE_NULL);
            return false;
        }
    }
    gst_element_set_state(sink, GST_STATE_NULL);
    return true;
}

// A real audio sink for the system's default output, or nullptr when no device
// can be opened. autoaudiosink is preferred (it adapts to PulseAudio/PipeWire/
// ALSA); when it falls back to its internal fake, the explicit backends are
// tried in order. Each candidate is opened for validation and handed back in
// NULL state, ready for the pipeline to take over; the caller owns it.
GstElement *make_real_audio_sink()
{
    static const char *const kFactories[] = {
        "autoaudiosink",
        "pipewiresink",
        "pulsesink",
        nullptr,
    };
    for (const char *const *name = kFactories; *name != nullptr; ++name) {
        GstElement *sink = gst_element_factory_make(*name, nullptr);
        if (sink == nullptr) {
            continue;
        }
        g_object_set(sink, "sync", TRUE, nullptr);
        if (audio_sink_opens(sink)) {
            return sink;
        }
        gst_object_unref(sink);
    }
    return nullptr;
}

// Builds the session's GES pipeline over `built` and prerolls to PAUSED. When
// `want_real` a real audio sink is probed and wired, else a clocked fakesink
// drains audio silently. `presenting` selects the GL upload chain and its bus
// sync handler. Returns the prerolled pipeline (caller owns) or nullptr. The
// preview sinks are consumed by preview_set_*_sink (the pipeline releases them
// on teardown), matching the ownership the session always used.
GESPipeline *build_pipeline(const genesis::render::BuiltTimeline &built, bool want_real,
                            GesSession::Presentation *presentation, bool presenting)
{
    GESTimeline *timeline = build_timeline(built);
    if (timeline == nullptr) {
        return nullptr;
    }
    // build_timeline returns a floating reference; take ownership of it so the
    // reference counts below are explicit rather than borrowed.
    gst_object_ref_sink(timeline);

    GESPipeline *pipeline = ges_pipeline_new();
    if (pipeline == nullptr) {
        gst_object_unref(timeline);
        return nullptr;
    }
    gst_object_ref_sink(pipeline);

    // Install the context-answering sync handler before the preview sinks are
    // wired and the mode is entered: GES triggers the preview's context
    // negotiation as soon as those happen, so a handler set after them misses
    // the NEED_CONTEXT messages and the GL elements create their own context.
    if (presenting) {
        GstBus *bus = gst_element_get_bus(GST_ELEMENT(pipeline));
        gst_bus_set_sync_handler(bus, on_bus_sync, presentation, nullptr);
        gst_object_unref(bus);
    }

    // set_timeline adds the timeline as a child (gst_bin_add), so the pipeline
    // owns it from here; drop our reference.
    if (!ges_pipeline_set_timeline(pipeline, timeline)) {
        gst_object_unref(pipeline);
        gst_object_unref(timeline);
        return nullptr;
    }
    gst_object_unref(timeline);

    // Preview mode is the default already; the call documents intent and is a
    // no-op fast path rather than a state change.
    if (!ges_pipeline_set_mode(pipeline, GES_PIPELINE_MODE_PREVIEW)) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    // Video: when the host adopted a GL context, present frames through the
    // GL upload chain; otherwise the session stays a pure clock/graph driver
    // and drains into a fakesink so it runs without a display.
    GstElement *video_sink = nullptr;
    if (presenting) {
        video_sink = build_present_sink(presentation);
        if (video_sink == nullptr) {
            gst_object_unref(pipeline);
            return nullptr;
        }
    } else {
        video_sink = gst_element_factory_make("fakesink", nullptr);
        g_object_set(video_sink, "sync", TRUE, nullptr);
    }

    // Audio: a real sink when requested and one opens, else a clocked fakesink
    // so a machine with no sound device still plays pictures.
    GstElement *audio_sink = want_real ? make_real_audio_sink() : nullptr;
    if (audio_sink == nullptr) {
        audio_sink = make_audio_fakesink();
    }
    if (video_sink == nullptr || audio_sink == nullptr) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    // preview_set_*_sink require NULL state (set_mode left it there) and take
    // ownership of the sinks; do not unref them from here.
    ges_pipeline_preview_set_video_sink(pipeline, video_sink);
    ges_pipeline_preview_set_audio_sink(pipeline, audio_sink);

    // Preroll: PAUSED readies the first frame so position() and seek() answer
    // immediately. A session that never prerolls cannot seek or report.
    //
    // An empty timeline has no data to preroll, so the state change never
    // completes and block_until_state would sit out its full timeout - on the
    // UI thread, which is what makes New Project look hung. Skip the wait when
    // there is nothing to ready; the pipeline is still valid and a later load
    // with clips prerolls normally.
    if (built.timeline.clip_count() == 0) {
        return pipeline;
    }
    GstState settled = GST_STATE_VOID_PENDING;
    GstStateChangeReturn result = GST_STATE_CHANGE_FAILURE;
    if (!block_until_state_verbose(GST_ELEMENT(pipeline), GST_STATE_PAUSED, &settled, &result)) {
        const std::string bus_error = drain_bus(GST_ELEMENT(pipeline));
        // Name the outcome: a timeout (ASYNC after the budget) is a negotiation
        // stall, a FAILURE is a hard error. The settled state and the bus
        // messages (error/warning/info) make the difference diagnosable — a
        // decoder-less stream surfaces its missing-plugin warning here.
        g_warning("GesSession::build_pipeline: preroll to PAUSED failed "
                  "(clips=%zu, result=%s, settled=%s)%s%s",
                  static_cast<size_t>(built.timeline.clip_count()),
                  result == GST_STATE_CHANGE_ASYNC ? "timeout" : "failure",
                  gst_element_state_get_name(settled), bus_error.empty() ? "" : ": ",
                  bus_error.c_str());
        // A failed preroll leaves the pipeline mid-transition (PAUSED or
        // PAUSED->PLAYING). Dropping the last reference from there disposes its
        // children while they are still in PAUSED, which GStreamer reports as a
        // CRITICAL refcount/state error. Force NULL first so every element
        // tears down cleanly.
        gst_element_set_state(GST_ELEMENT(pipeline), GST_STATE_NULL);
        gst_object_unref(pipeline);
        return nullptr;
    }

    return pipeline;
}

} // namespace

GesSession::GesSession() = default;

GesSession::~GesSession()
{
    if (pipeline_ != nullptr) {
        // Detach the appsink callback first: a wedged decoder can keep the
        // streaming thread alive past the state-change wait below, and a late
        // frame must not call into the host being destroyed.
        detach_presentation();
        gst_element_set_state(GST_ELEMENT(pipeline_), GST_STATE_NULL);
        GstState settled = GST_STATE_VOID_PENDING;
        gst_element_get_state(GST_ELEMENT(pipeline_), &settled, nullptr, 5 * GST_SECOND);
        gst_object_unref(pipeline_);
        // A wedged element can leave the streaming thread running past the
        // wait. The callback is detached, but the thread still holds the
        // presentation as its user_data, so freeing it here would be a
        // use-after-free. Leak it instead: the process is exiting, and a
        // bounded leak is preferable to a crash on shutdown.
        if (settled != GST_STATE_NULL) {
            (void)presentation_.release();
        }
    }
}

// Marks the presentation detached and clears the appsink callback, so no frame
// delivered after this point reaches the host. Idempotent; safe to call before
// every pipeline teardown.
void GesSession::detach_presentation()
{
    if (presentation_ == nullptr) {
        return;
    }
    presentation_->detached.store(true, std::memory_order_release);
    if (presentation_->appsink != nullptr) {
        GstAppSinkCallbacks empty = { };
        gst_app_sink_set_callbacks(GST_APP_SINK(presentation_->appsink), &empty, nullptr, nullptr);
    }
}

bool GesSession::load(const genesis::render::BuiltTimeline &built)
{
    // A reload starts clean: tear down whatever played before, releasing the
    // previous pipeline and the audio/video sinks it owned. The NULL transition
    // is asynchronous, so wait for it to settle before building the next
    // pipeline: the old elements release the shared GL display/context on their
    // streaming threads, and a new preroll that races that teardown can stall
    // until its timeout. A bounded wait keeps a wedged element from hanging the
    // reload.
    if (pipeline_ != nullptr) {
        // Detach the old appsink callback before the pipeline is dropped: a
        // wedged decoder can keep the old streaming thread alive past the wait
        // below, and a late frame must not call into the host.
        detach_presentation();
        gst_element_set_state(GST_ELEMENT(pipeline_), GST_STATE_NULL);
        gst_element_get_state(GST_ELEMENT(pipeline_), nullptr, nullptr, 5 * GST_SECOND);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }

    const bool presenting = presentation_ != nullptr && presentation_->context != nullptr;
    const bool want_real = audio_output_requested_;

    // A new timeline starts at zero: clear the presentation floor so the first
    // frame of the reloaded pipeline is not dropped as "behind" the old one.
    // Re-arm the callback: the new pipeline's appsink is attached fresh below.
    if (presentation_ != nullptr) {
        presentation_->pts_floor.store(GST_CLOCK_TIME_NONE, std::memory_order_relaxed);
        presentation_->detached.store(false, std::memory_order_release);
        if (presentation_->appsink != nullptr) {
            gst_object_unref(presentation_->appsink);
            presentation_->appsink = nullptr;
        }
    }

    empty_ = built.timeline.clip_count() == 0;

    GESPipeline *pipeline = build_pipeline(built, want_real, presentation_.get(), presenting);
    if (pipeline != nullptr) {
        pipeline_ = pipeline;
        return true;
    }

    // The real sink could not carry the pipeline (the device may have become
    // unavailable after the probe). Rebuild once with a clocked fakesink so
    // video still plays.
    if (want_real) {
        GESPipeline *retry =
                build_pipeline(built, /*want_real=*/false, presentation_.get(), presenting);
        if (retry != nullptr) {
            pipeline_ = retry;
            return true;
        }
    }
    // Both attempts failed. The caller only sees `false`; record why so a
    // "reload failed" in the app log is diagnosable without a debugger.
    g_warning("GesSession::load: pipeline build failed (clips=%zu, presenting=%d, "
              "real_audio=%d)",
              static_cast<size_t>(built.timeline.clip_count()), presenting ? 1 : 0,
              want_real ? 1 : 0);
    return false;
}

bool GesSession::play()
{
    if (pipeline_ == nullptr) {
        g_warning("GesSession::play: no pipeline loaded");
        return false;
    }
    // An empty pipeline never reaches PLAYING (nothing to preroll), so the
    // blocking wait would sit out its full timeout. Report success without it:
    // there is nothing to play, and the next load with clips plays normally.
    if (empty_) {
        return true;
    }
    return block_until_state(GST_ELEMENT(pipeline_), GST_STATE_PLAYING);
}

bool GesSession::pause()
{
    if (pipeline_ == nullptr) {
        g_warning("GesSession::pause: no pipeline loaded");
        return false;
    }
    if (empty_) {
        return true;
    }
    return block_until_state(GST_ELEMENT(pipeline_), GST_STATE_PAUSED);
}

bool GesSession::seek(Rational position)
{
    if (pipeline_ == nullptr) {
        g_warning("GesSession::seek: no pipeline loaded");
        return false;
    }
    // Raise the presentation floor to the seek target before the flush, so a
    // frame from the old position that arrives after the target is dropped
    // rather than flashing the picture backward (see deliver_sample). The
    // tolerance is one frame at 24fps: an accurate seek lands on the frame
    // containing the target, whose PTS can sit just before it.
    if (presentation_ != nullptr) {
        constexpr GstClockTime kSeekFloorTolerance = 42 * GST_MSECOND;
        const GstClockTime target = to_ns(position);
        presentation_->pts_floor.store(target > kSeekFloorTolerance ? target - kSeekFloorTolerance
                                                                    : 0,
                                       std::memory_order_relaxed);
    }
    const gboolean ok = gst_element_seek(
            GST_ELEMENT(pipeline_), 1.0, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            GST_SEEK_TYPE_SET, to_ns(position), GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    return ok != FALSE;
}

Rational GesSession::position() const
{
    if (pipeline_ == nullptr) {
        return Rational::ZERO;
    }
    gint64 ns = GST_CLOCK_TIME_NONE;
    if (!gst_element_query_position(GST_ELEMENT(pipeline_), GST_FORMAT_TIME, &ns)) {
        return Rational::ZERO;
    }
    return from_ns(static_cast<GstClockTime>(ns));
}

bool GesSession::prepare(Rational position)
{
    if (pipeline_ == nullptr) {
        return false;
    }

    // A position past the timeline's end has no frame to warm. Reject it up
    // front, before the state is touched, so a playing session is left
    // rolling: a flush seek past the end clamps unpredictably and its preroll
    // cannot be trusted to say "no frame here".
    gint64 duration = GST_CLOCK_TIME_NONE;
    if (gst_element_query_duration(GST_ELEMENT(pipeline_), GST_FORMAT_TIME, &duration)
        && duration != static_cast<gint64>(GST_CLOCK_TIME_NONE)
        && to_ns(position) > static_cast<GstClockTime>(duration)) {
        return false;
    }

    // Remember whether the session was rolling so it can be put back exactly
    // as it was; prepare must be invisible to a caller that is already
    // playing. A pending PLAYING counts as playing: the session is rolling or
    // about to.
    GstState current = GST_STATE_VOID_PENDING;
    GstState pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(GST_ELEMENT(pipeline_), &current, &pending, 0);
    const bool was_playing = current == GST_STATE_PLAYING || pending == GST_STATE_PLAYING;

    // Settle to PAUSED first so the flush seek below starts from a known
    // state rather than mid-roll.
    if (!block_until_state(GST_ELEMENT(pipeline_), GST_STATE_PAUSED)) {
        return false;
    }

    // The same accurate flush seek seek() uses, then block until the pipeline
    // prerolls at the target. block_until_state's bounded get_state is the
    // "do not hang on a broken position" guarantee.
    const gboolean sought = gst_element_seek(
            GST_ELEMENT(pipeline_), 1.0, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
            GST_SEEK_TYPE_SET, to_ns(position), GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    const bool warmed =
            sought != FALSE && block_until_state(GST_ELEMENT(pipeline_), GST_STATE_PAUSED);

    // Put the session back the way it was: a playing session keeps playing, a
    // paused one stays paused. The restore is best-effort - the caller already
    // has the warmed answer, and a failed restore leaves the session paused
    // rather than wrongly rolling.
    if (was_playing) {
        block_until_state(GST_ELEMENT(pipeline_), GST_STATE_PLAYING);
    }

    return warmed;
}

bool GesSession::set_gl_context(genesis::adapters::engine::GlHandles handles)
{
    // Zero means "not set": there is nothing to adopt. The GL upload chain
    // needs both halves of the pair.
    if (handles.display == 0 || handles.context == 0) {
        return false;
    }

    // Wrap the app's EGL display and context so the GL elements render into
    // the app's context. Wrapped, not owned: the app keeps the native objects
    // alive for as long as the session runs. The wrapped context must declare
    // the app's actual client API; a mismatch (desktop GL app wrapped as GLES2)
    // yields textures the app's context samples black even though the handle is
    // valid. The host detects the API from its own context and passes it here.
    GstGLDisplay *display = reinterpret_cast<GstGLDisplay *>(
            gst_gl_display_egl_new_with_egl_display(reinterpret_cast<gpointer>(handles.display)));
    if (display == nullptr) {
        return false;
    }
    const GstGLAPI api = gst_gl_api_from(handles.api);
    GstGLContext *wrapped = gst_gl_context_new_wrapped(
            display, static_cast<guintptr>(handles.context), GST_GL_PLATFORM_EGL, api);
    if (wrapped == nullptr) {
        gst_object_unref(display);
        return false;
    }

    // Hand the wrapped app context straight to the pipeline. The GL elements
    // activate this exact context, so they render on the app's own desktop-GL
    // context and the texture id the frame sink hands out is valid in the RHI
    // context that samples it.
    //
    // Do NOT create a second GStreamer-owned context via
    // gst_gl_display_create_context: on the EGL platform that path cannot read
    // the config off a wrapped context (gst_gl_wrapped_context_get_config is a
    // no-op) and falls back to creating an OpenGL ES context from its own
    // defaults, discarding the API declared here. The pipeline would then
    // render on an ES context whose textures the desktop-GL RHI cannot sample -
    // valid texture handle, black readback.
    if (presentation_ == nullptr) {
        presentation_ = std::make_unique<Presentation>();
    }
    // Replace any previously adopted context; the new one wins.
    if (presentation_->context != nullptr) {
        gst_object_unref(presentation_->context);
    }
    if (presentation_->display != nullptr) {
        gst_object_unref(presentation_->display);
    }
    presentation_->display = display;
    presentation_->context = wrapped;
    return true;
}

void GesSession::set_frame_sink(genesis::adapters::engine::FrameSink sink)
{
    if (presentation_ == nullptr) {
        presentation_ = std::make_unique<Presentation>();
    }
    presentation_->sink = std::move(sink);
}

bool GesSession::set_audio_output(bool enabled)
{
    // Disabling is always a valid, working configuration: nothing can fail.
    if (!enabled) {
        audio_output_requested_ = false;
        return true;
    }

    // Probe whether a real output can be opened now, so the caller gets an
    // honest answer and load() only arms a sink that was known to open. The
    // sink is validated and discarded here; load() re-creates it.
    GstElement *sink = make_real_audio_sink();
    if (sink == nullptr) {
        audio_output_requested_ = false;
        return false;
    }
    gst_object_unref(sink);
    audio_output_requested_ = true;
    return true;
}

bool GesSession::set_decode_preference(genesis::adapters::engine::DecodePreference preference)
{
    // Re-tilt the process-global decoder ranking through the shared policy. The
    // policy restores the shipped ranks first, so this is idempotent across the
    // startup apply and every preference change.
    HardwareInfo probe;
    probe.hardware_decode = hardware_decode_available();
    const DecodePolicyResult result = apply_decode_policy(preference, probe);

    // The preference tilts the decoder ranking before the pipeline negotiates,
    // so a pipeline that has already loaded has already made its decoder
    // choices and the retilt above takes effect only on the next load().
    // Returning false (rather than rebuilding) is the honest answer: the
    // session keeps no BuiltTimeline, so it cannot reload on its own, and the
    // caller must set the preference on a fresh session before load().
    if (pipeline_ != nullptr) {
        return false;
    }
    return result.honoured;
}

} // namespace genesis::adapters::engine::ges
