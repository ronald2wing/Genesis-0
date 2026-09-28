// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/Export.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include <ges/ges.h>
#include <gst/gst.h>
#include <gst/pbutils/encoding-profile.h>

#include "adapters/engine/ges/export/Encoder.h"
#include "adapters/engine/ges/session/GesBuilder.h"
#include "render/ExportSpec.h"
#include "render/Resolve.h"

namespace genesis::adapters::engine {

namespace {

using genesis::core::FrameRate;
using genesis::core::Rational;
using genesis::core::TrackKind;
using genesis::render::AudioCodec;
using genesis::render::BuiltTimeline;
using genesis::render::ExportCodec;
using genesis::render::ExportSpec;

// How often the render's tick source wakes: to check for a cancellation
// request and report progress. Short enough that a cancel lands promptly, long
// enough that the loop does not busy-spin.
constexpr GstClockTime kPollInterval = 50 * GST_MSECOND;

// How long past the timeline's own span the render may run before it is
// declared stuck. Render mode is free-running (usually faster than real time),
// so the span plus this margin is a generous deadline that only a genuinely
// stuck render can outlive.
constexpr GstClockTime kWatchdogMargin = 60 * GST_SECOND;

// The exact rational -> GES nanosecond seam, matching to_ns in GesBuilder.cpp
// and GesSession.cpp: as_double() is the documented lossy engine-argument
// edge, never converted back.
GstClockTime to_ns(Rational time)
{
    return static_cast<GstClockTime>(std::llround(time.as_double() * GST_SECOND));
}

// The reverse seam: nanoseconds are already an exact integer, so the rational
// is exact and only reduces to lowest terms.
Rational from_ns(GstClockTime ns)
{
    return Rational(static_cast<std::int64_t>(ns), static_cast<std::int64_t>(GST_SECOND));
}

// The encoder, container and encoded-stream caps for one codec family. The
// container caps strings mirror GStreamer's own file-extension encoding
// targets. `candidates` names the elements in preference order as
// ges::EncoderCandidate values (Encoder.h, shared with the status probe so the
// indicator agrees with what export picks); each carries the raw pixel format
// its sink accepts for eight- and ten-bit input.
struct CodecPlan
{
    std::span<const ges::EncoderCandidate> candidates; // ordered preference
    const char *container; // container caps, picks the muxer
    const char *stream; // encoded stream caps, picks the encoder
    const char *name; // host-facing name, for legible errors
};

const CodecPlan *codec_plan(ExportCodec codec)
{
    switch (codec) {
    case ExportCodec::Vp8: {
        static const std::array<ges::EncoderCandidate, 1> candidates = { ges::EncoderCandidate{
                "vp8enc", nullptr, nullptr, false } };
        static const CodecPlan plan{ std::span<const ges::EncoderCandidate>(candidates),
                                     "video/webm", "video/x-vp8", "vp8" };
        return &plan;
    }
    case ExportCodec::Vp9: {
        static const std::array<ges::EncoderCandidate, 1> candidates = { ges::EncoderCandidate{
                "vp9enc", nullptr, nullptr, false } };
        static const CodecPlan plan{ std::span<const ges::EncoderCandidate>(candidates),
                                     "video/webm", "video/x-vp9", "vp9" };
        return &plan;
    }
    case ExportCodec::Av1: {
        static const std::array<ges::EncoderCandidate, 1> candidates = { ges::EncoderCandidate{
                "av1enc", nullptr, nullptr, false } };
        static const CodecPlan plan{ std::span<const ges::EncoderCandidate>(candidates),
                                     "video/webm", "video/x-av1", "av1" };
        return &plan;
    }
    case ExportCodec::H264: {
        static const std::array<ges::EncoderCandidate, 1> candidates = { ges::EncoderCandidate{
                "openh264enc", nullptr, nullptr, false } };
        static const CodecPlan plan{ std::span<const ges::EncoderCandidate>(candidates),
                                     "video/quicktime", "video/x-h264", "h264" };
        return &plan;
    }
    case ExportCodec::Hevc: {
        // The shared hardware-first list (ges::kH265Encoders): the GPU
        // encoders in preference order, then the software x265 fallback.
        // Each format name comes from that element's own sink template.
        static const CodecPlan plan{ std::span<const ges::EncoderCandidate>(ges::kH265Encoders),
                                     "video/quicktime", "video/x-h265", "hevc" };
        return &plan;
    }
    case ExportCodec::Theora: {
        static const std::array<ges::EncoderCandidate, 1> candidates = { ges::EncoderCandidate{
                "theoraenc", nullptr, nullptr, false } };
        static const CodecPlan plan{ std::span<const ges::EncoderCandidate>(candidates),
                                     "application/ogg", "video/x-theora", "theora" };
        return &plan;
    }
    }
    return nullptr;
}

// The candidate a plan answers with on this machine, or nullptr. Binds the
// real registry to the shared preference logic (ges::resolve_encoder): the
// element factory must be present and, for a ten-bit request, its sink must
// accept the candidate's ten-bit format. On success `raw_format` is the raw
// pixel format the render must pin (null when encodebin negotiates freely); on
// failure `error` names the refusal.
const ges::EncoderCandidate *resolve_encoder(const CodecPlan &plan, bool ten_bit,
                                             const char **raw_format, std::string &error)
{
    const auto usable = [](const char *name) {
        GstElementFactory *factory = gst_element_factory_find(name);
        if (factory == nullptr) {
            return false;
        }
        gst_object_unref(factory);
        return true;
    };
    const auto accepts = [](const char *name, const char *format) {
        return ges::sink_accepts(name, format);
    };
    return ges::resolve_encoder(plan.candidates, ten_bit, raw_format, error, plan.name, usable,
                                accepts);
}

// The encoder and encoded-stream caps for one audio codec family. Mirrors the
// video plan: the host names the family, the engine names the element.
struct AudioPlan
{
    const char *encoder;
    const char *stream;
    const char *name;
};

const AudioPlan *audio_plan(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Vorbis: {
        static const AudioPlan plan{ "vorbisenc", "audio/x-vorbis", "vorbis" };
        return &plan;
    }
    case AudioCodec::Opus: {
        static const AudioPlan plan{ "opusenc", "audio/x-opus", "opus" };
        return &plan;
    }
    case AudioCodec::Aac: {
        static const AudioPlan plan{ "fdkaacenc", "audio/mpeg, mpegversion=4", "aac" };
        return &plan;
    }
    case AudioCodec::Flac: {
        static const AudioPlan plan{ "flacenc", "audio/x-flac", "flac" };
        return &plan;
    }
    }
    return nullptr;
}

// Whether the host graph carries any audio: a track of kind Audio. The host's
// resolver emits audio tracks, so this is true whenever the timeline has one -
// which is what gates the audio profile, so encodebin never waits for sound
// that will not come.
bool has_audio(const BuiltTimeline &built)
{
    for (const auto &track_id : built.timeline.track_ids()) {
        const auto *track = built.timeline.track(track_id);
        if (track != nullptr && track->kind() == TrackKind::Audio) {
            return true;
        }
    }
    return false;
}

// One render run's shared state, driven by a GLib GMainLoop. GES's render work
// advances from the main context (build_timeline already proves this for asset
// loads: a bare sync request returns null and needs a loop), so a bus poll
// without one stalls - nothing iterates the context GES schedules its async
// state changes, ASYNC_DONE and EOS on. The bus watch turns EOS/ERROR into a
// quit, the tick polls cancellation and reports progress from the position
// query, and the watchdog bounds the run so a stuck render fails loudly.
struct RenderDrive
{
    GMainLoop *loop = nullptr;
    GstElement *pipeline = nullptr;
    GstClockTime total_ns = 0;
    bool ok = false;
    bool playing = false;
    std::string error;
    std::function<void(ExportProgress)> progress;
    std::function<bool()> cancelled;
};

gboolean on_bus_message(GstBus *bus, GstMessage *message, gpointer user_data)
{
    (void)bus;
    auto *drive = static_cast<RenderDrive *>(user_data);
    switch (GST_MESSAGE_TYPE(message)) {
    case GST_MESSAGE_EOS:
        drive->ok = true;
        g_main_loop_quit(drive->loop);
        break;
    case GST_MESSAGE_ERROR: {
        GError *gerror = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(message, &gerror, &debug);
        drive->error = gerror != nullptr ? gerror->message : "render failed";
        g_clear_error(&gerror);
        g_free(debug);
        g_main_loop_quit(drive->loop);
        break;
    }
    case GST_MESSAGE_STATE_CHANGED: {
        if (GST_MESSAGE_SRC(message) == GST_OBJECT(drive->pipeline)) {
            GstState old_state = GST_STATE_VOID_PENDING;
            GstState new_state = GST_STATE_VOID_PENDING;
            GstState pending = GST_STATE_VOID_PENDING;
            gst_message_parse_state_changed(message, &old_state, &new_state, &pending);
            if (new_state == GST_STATE_PLAYING) {
                drive->playing = true;
            }
        }
        break;
    }
    default:
        break;
    }
    return TRUE;
}

gboolean on_tick(gpointer user_data)
{
    auto *drive = static_cast<RenderDrive *>(user_data);
    if (drive->cancelled && drive->cancelled()) {
        drive->error = "cancelled";
        g_main_loop_quit(drive->loop);
        return G_SOURCE_REMOVE;
    }
    // The position query answers once the pipeline is in PLAYING and reports
    // the render's actual written position (it advances faster than wall time
    // because render mode is free-running, not clocked to the wall).
    if (drive->playing && drive->progress) {
        gint64 position_ns = 0;
        if (gst_element_query_position(drive->pipeline, GST_FORMAT_TIME, &position_ns)) {
            const double fraction = drive->total_ns > 0
                    ? std::clamp(static_cast<double>(position_ns)
                                         / static_cast<double>(drive->total_ns),
                                 0.0, 1.0)
                    : 1.0;
            drive->progress(
                    ExportProgress{ fraction, from_ns(static_cast<GstClockTime>(position_ns)) });
        }
    }
    return G_SOURCE_CONTINUE;
}

gboolean on_watchdog(gpointer user_data)
{
    auto *drive = static_cast<RenderDrive *>(user_data);
    drive->error = "render timed out";
    g_main_loop_quit(drive->loop);
    return G_SOURCE_REMOVE;
}

} // namespace

ExportOutcome export_timeline(const ExportSpec &spec, const BuiltTimeline &built,
                              const std::function<void(ExportProgress)> &progress,
                              const std::function<bool()> &cancelled)
{
    const CodecPlan *plan = codec_plan(spec.codec);
    if (plan == nullptr) {
        return ExportOutcome{ false, "unknown codec family", Rational::ZERO };
    }

    // Runtime refusal, not host validation: only the engine can see its own
    // registry (R5). The plan names a family; the element that answers it is
    // found here, preferring the hardware encoders and falling back through the
    // software one. A ten-bit request needs an element whose sink takes a
    // ten-bit raw format; an HDR timeline needs ten bits, so `hdr` forces the
    // same requirement. A family with no installed encoder - or no encoder
    // that answers the requested depth - refuses here rather than silently
    // downgrading.
    const bool ten_bit = spec.ten_bit || spec.hdr;
    const char *raw_format = nullptr;
    std::string resolve_error;
    const ges::EncoderCandidate *candidate =
            resolve_encoder(*plan, ten_bit, &raw_format, resolve_error);
    if (candidate == nullptr) {
        return ExportOutcome{ false, std::move(resolve_error), Rational::ZERO };
    }

    const Rational duration = built.timeline.duration();

    GESTimeline *ges_timeline = ges::build_timeline(built);
    if (ges_timeline == nullptr) {
        return ExportOutcome{ false, "failed to build the GES timeline", Rational::ZERO };
    }

    // Pin the video track to the requested frame and rate, so the render
    // scales to the output size rather than the source's native size. The
    // track copies the caps (const parameter), so ours is released after.
    {
        const FrameRate rate = built.timeline.frame_rate();
        GstCaps *restriction = gst_caps_new_simple(
                "video/x-raw", "width", G_TYPE_INT, static_cast<gint>(built.timeline.width()),
                "height", G_TYPE_INT, static_cast<gint>(built.timeline.height()), "framerate",
                GST_TYPE_FRACTION, static_cast<gint>(rate.fps().numerator()),
                static_cast<gint>(rate.fps().denominator()), nullptr);
        for (GList *node = ges_timeline_get_tracks(ges_timeline); node != nullptr;
             node = node->next) {
            auto *track = static_cast<GESTrack *>(node->data);
            if (track->type == GES_TRACK_TYPE_VIDEO) {
                ges_track_set_restriction_caps(track, restriction);
            }
        }
        gst_caps_unref(restriction);
    }

    // Build the encoding profile: a container plus one video stream. The
    // container and stream caps are owned by the profile once handed over, so
    // they are never unref'd here.
    GstEncodingContainerProfile *container = gst_encoding_container_profile_new(
            "genesis-export", "Genesis export", gst_caps_from_string(plan->container), nullptr);
    if (container == nullptr) {
        gst_object_unref(ges_timeline);
        return ExportOutcome{ false, "failed to build the container profile", Rational::ZERO };
    }
    // A ten-bit request pins the encoder's input to its ten-bit raw format
    // here, so encodebin converts to that depth at the encoder boundary rather
    // than negotiating an 8-bit input the encoder would silently widen. The
    // profile takes ownership of the caps once handed over. HDR requires ten
    // bits (the `spec.hdr` term in `ten_bit` above); the BT.2020/HLG/PQ
    // container signalling is the encoder's option-string concern, not a raw
    // pixel-format one.
    GstCaps *raw_restriction = nullptr;
    if (raw_format != nullptr) {
        raw_restriction =
                gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, raw_format, nullptr);
    }
    GstEncodingVideoProfile *video = gst_encoding_video_profile_new(
            gst_caps_from_string(plan->stream), nullptr, raw_restriction, 0);
    if (video == nullptr) {
        gst_encoding_profile_unref(container);
        gst_object_unref(ges_timeline);
        return ExportOutcome{ false, "failed to build the video profile", Rational::ZERO };
    }
    // Pin the resolved encoder: the profile's preset name names the exact
    // element factory, so encodebin cannot fall back to a higher-ranked but
    // broken hardware encoder (e.g. nvh265enc with no GPU) over the software
    // encoder the plan prefers. This is what makes the preference order in
    // `codec_plan` authoritative rather than a suggestion.
    gst_encoding_profile_set_preset_name(reinterpret_cast<GstEncodingProfile *>(video),
                                         candidate->element);
    gst_encoding_container_profile_add_profile(container,
                                               reinterpret_cast<GstEncodingProfile *>(video));

    // An audio stream only when the timeline has audio: an audio profile over
    // a silent timeline makes encodebin wait for sound that never comes.
    if (has_audio(built)) {
        const AudioPlan *aplan = audio_plan(spec.audio_codec);
        if (aplan != nullptr) {
            GstEncodingAudioProfile *audio = gst_encoding_audio_profile_new(
                    gst_caps_from_string(aplan->stream), nullptr, nullptr, 0);
            if (audio != nullptr) {
                gst_encoding_container_profile_add_profile(
                        container, reinterpret_cast<GstEncodingProfile *>(audio));
            }
        }
    }

    GESPipeline *pipeline = ges_pipeline_new();
    if (pipeline == nullptr) {
        gst_encoding_profile_unref(container);
        gst_object_unref(ges_timeline);
        return ExportOutcome{ false, "failed to create the GES pipeline", Rational::ZERO };
    }

    // build_timeline hands back a floating reference: sink it, hand the
    // pipeline its own reference, then release ours. The pipeline owns the
    // timeline for the rest of the run.
    gst_object_ref_sink(ges_timeline);
    if (!ges_pipeline_set_timeline(pipeline, ges_timeline)) {
        gst_object_unref(ges_timeline);
        gst_object_unref(pipeline);
        gst_encoding_profile_unref(container);
        return ExportOutcome{ false, "failed to attach the timeline to the pipeline",
                              Rational::ZERO };
    }
    gst_object_unref(ges_timeline);

    // Render settings must precede the mode switch: ges_pipeline_set_mode()
    // builds the render bin only when an output URI is already configured, and
    // returns FALSE otherwise (the "Output URI not set !" refusal). Reversing
    // them leaves the pipeline half-configured in preview mode with no sink to
    // drain, so it never posts EOS. The mode's return is checked rather than
    // ignored for the same reason.
    gchar *output_uri = gst_filename_to_uri(spec.output.c_str(), nullptr);
    if (output_uri == nullptr
        || !ges_pipeline_set_render_settings(pipeline, output_uri,
                                             reinterpret_cast<GstEncodingProfile *>(container))) {
        g_free(output_uri);
        gst_object_unref(pipeline);
        gst_encoding_profile_unref(container);
        return ExportOutcome{ false, "failed to configure the render", Rational::ZERO };
    }
    g_free(output_uri);
    if (!ges_pipeline_set_mode(pipeline, GES_PIPELINE_MODE_RENDER)) {
        gst_object_unref(pipeline);
        gst_encoding_profile_unref(container);
        return ExportOutcome{ false, "failed to enter render mode", Rational::ZERO };
    }

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    GstElement *element = GST_ELEMENT(pipeline);

    RenderDrive drive;
    drive.pipeline = element;
    drive.total_ns = to_ns(duration);
    drive.progress = progress;
    drive.cancelled = cancelled;

    // A dedicated context keeps the export self-contained on whatever worker
    // thread runs it; the bus watch and the tick/watchdog sources all attach
    // to it, so g_main_loop_run iterates exactly the sources GES needs.
    GMainContext *context = g_main_context_new();
    drive.loop = g_main_loop_new(context, FALSE);

    // The bus watch attaches to the thread-default context, so pin that around
    // the add and pop it straight back.
    g_main_context_push_thread_default(context);
    gst_bus_add_watch(bus, on_bus_message, &drive);
    g_main_context_pop_thread_default(context);

    GSource *tick = g_timeout_source_new(kPollInterval / GST_MSECOND);
    g_source_set_callback(tick, on_tick, &drive, nullptr);
    g_source_attach(tick, context);
    g_source_unref(tick);

    // The bounded-wait guarantee: render mode is free-running (it typically
    // finishes faster than real time), so the timeline's span plus a fixed
    // margin is a generous deadline that only a genuinely stuck render can
    // outlive. If it does, the watchdog quits the loop with an error rather
    // than spinning forever.
    const guint watchdog_seconds =
            static_cast<guint>((drive.total_ns + kWatchdogMargin) / GST_SECOND);
    GSource *watchdog = g_timeout_source_new_seconds(watchdog_seconds);
    g_source_set_callback(watchdog, on_watchdog, &drive, nullptr);
    g_source_attach(watchdog, context);
    g_source_unref(watchdog);

    // The watch is in place before PLAYING, so a fast EOS cannot slip past it.
    if (gst_element_set_state(element, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        g_main_loop_unref(drive.loop);
        g_main_context_unref(context);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        gst_encoding_profile_unref(container);
        std::filesystem::remove(spec.output);
        return ExportOutcome{ false, "failed to start the render", Rational::ZERO };
    }

    g_main_loop_run(drive.loop);

    const bool ok = drive.ok;
    const std::string error = drive.error;

    // The bus watch, tick and watchdog are all attached to `context`; dropping
    // the last reference destroys them, so there is no per-source removal to
    // get wrong (g_source_remove only reaches the default context).
    g_main_loop_unref(drive.loop);
    g_main_context_unref(context);

    gst_element_set_state(element, GST_STATE_NULL);

    // A clean end is exactly one fraction's worth of timeline, whatever the
    // last polled position was short of it.
    if (ok && progress) {
        progress(ExportProgress{ 1.0, duration });
    }
    if (!ok) {
        std::filesystem::remove(spec.output);
    }

    gst_object_unref(bus);
    gst_object_unref(pipeline);
    gst_encoding_profile_unref(container);

    return ExportOutcome{ ok, error, ok ? duration : Rational::ZERO };
}

} // namespace genesis::adapters::engine
