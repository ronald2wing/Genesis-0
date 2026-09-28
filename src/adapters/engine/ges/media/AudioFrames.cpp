// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/AudioFrames.h"

#include <cstdint>
#include <optional>
#include <string>
#include <system_error>

#include <gst/gst.h>

#include "adapters/engine/ges/media/Probe.h"
#include "project/command/MediaCommands.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::project::NewMedia;

// 16 kHz mono float32: the fixed format the caption runtime reads (no header,
// 4 bytes per sample, little-endian). F32LE is the native float layout on the
// hosts this targets, so a raw file of these is exactly what WhisperProvider
// transcribe_file consumes.
constexpr gint kSampleRate = 16000;
constexpr gint kChannels = 1;

// Wall-clock bound on the extraction. A raw decode has no live element and so
// runs far faster than real time; this only guards against a wedged pipeline
// (a dead decoder, a stalled filesink), never a long-but-healthy file.
constexpr GstClockTime kExtractTimeout = 300 * GST_SECOND;

// How the decode finished: a clean end of stream, a posted error, or neither
// before the time bound.
enum class DecodeEnd { Eos, Error, Timeout };

// Whether a decodebin pad carries audio.
bool pad_is_audio(GstPad *pad)
{
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    if (caps == nullptr) {
        return false;
    }
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    const gchar *name = structure == nullptr ? nullptr : gst_structure_get_name(structure);
    const bool audio = name != nullptr && g_str_has_prefix(name, "audio/");
    gst_caps_unref(caps);
    return audio;
}

// The state the pad-added handler drives: it keeps the chosen audio stream
// (the target-th one, in file order) and routes every other stream to a
// fresh fakesink. A decodebin exposes a pad per stream, and a stream whose
// pad is left unlinked stalls or posts a not-linked error, so unselected
// streams must be consumed, not ignored.
struct PadSelect
{
    GstElement *pipeline = nullptr;
    GstElement *convert = nullptr;
    std::uint32_t target = 0; // which audio stream (file order) to keep
    std::uint32_t seen = 0; // audio streams met so far
    bool linked = false; // the target pad is already wired
};

void on_pad_added(GstElement *, GstPad *pad, gpointer user_data)
{
    auto *select = static_cast<PadSelect *>(user_data);

    const bool audio = pad_is_audio(pad);
    if (audio && select->seen == select->target && !select->linked) {
        ++select->seen;
        GstPad *sink = gst_element_get_static_pad(select->convert, "sink");
        if (sink != nullptr) {
            if (!gst_pad_is_linked(sink)) {
                gst_pad_link(pad, sink);
            }
            gst_object_unref(sink);
        }
        select->linked = true;
        return;
    }
    if (audio) {
        ++select->seen;
    }

    // Any other stream (video, subtitles, an unselected audio track) is
    // consumed by its own fakesink so it cannot stall the decode.
    GstElement *drop = gst_element_factory_make("fakesink", nullptr);
    if (drop == nullptr) {
        return;
    }
    g_object_set(drop, "sync", FALSE, nullptr);
    // gst_bin_add takes the floating reference, so the bin now owns the
    // fakesink and will set it back to NULL with the rest of the pipeline;
    // dropping our pointer here would dispose it while still PLAYING.
    gst_bin_add(GST_BIN(select->pipeline), drop);
    gst_element_sync_state_with_parent(drop);
    GstPad *sink = gst_element_get_static_pad(drop, "sink");
    if (sink != nullptr) {
        gst_pad_link(pad, sink);
        gst_object_unref(sink);
    }
}

// Decodes the chosen audio stream of `source` to `destination` as 16 kHz mono
// F32LE. The pipeline is uridecodebin -> audioconvert -> audioresample ->
// capsfilter -> filesink: uridecodebin is filesrc + decodebin in one element,
// so the source file opens and its audio decodes through the same path the
// repo's Proxy and Reverse use. `detail` receives the bus error's message when
// the pipeline posts one. Returns Eos only when the whole stream reached a
// clean end of stream.
DecodeEnd decode_to_pcm(const std::filesystem::path &source,
                        const std::filesystem::path &destination, std::uint32_t stream_index,
                        std::string &detail)
{
    gchar *uri = gst_filename_to_uri(source.string().c_str(), nullptr);
    if (uri == nullptr) {
        detail = "cannot turn the source path into a URI";
        return DecodeEnd::Error;
    }

    GstElement *pipeline = gst_pipeline_new("audio-extract");
    GstElement *decode = gst_element_factory_make("uridecodebin", "decode");
    GstElement *convert = gst_element_factory_make("audioconvert", "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *filter = gst_element_factory_make("capsfilter", "filter");
    GstElement *sink = gst_element_factory_make("filesink", "sink");
    if (pipeline == nullptr || decode == nullptr || convert == nullptr || resample == nullptr
        || filter == nullptr || sink == nullptr) {
        gst_clear_object(&pipeline);
        gst_clear_object(&decode);
        gst_clear_object(&convert);
        gst_clear_object(&resample);
        gst_clear_object(&filter);
        gst_clear_object(&sink);
        g_free(uri);
        detail = "a pipeline element is missing from the GStreamer registry";
        return DecodeEnd::Error;
    }

    g_object_set(decode, "uri", uri, nullptr);
    g_free(uri);

    // Pin the exact target format so audioconvert/audioresample produce it
    // rather than negotiate whatever the source offered.
    GstCaps *caps =
            gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE", "rate", G_TYPE_INT,
                                kSampleRate, "channels", G_TYPE_INT, kChannels, nullptr);
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    g_object_set(sink, "location", destination.string().c_str(), nullptr);

    gst_bin_add_many(GST_BIN(pipeline), decode, convert, resample, filter, sink, nullptr);
    if (!gst_element_link_many(convert, resample, filter, sink, nullptr)) {
        gst_object_unref(pipeline);
        detail = "cannot link the audio pipeline";
        return DecodeEnd::Error;
    }

    PadSelect select;
    select.pipeline = pipeline;
    select.convert = convert;
    select.target = stream_index;
    g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), &select);

    // Drive to EOS or error, bounded by the timeout. A filesink has no live
    // consumer, so no GMainLoop is needed: the bus's timed pop blocks until
    // the pipeline ends, posts an error, or the bound lapses.
    GstBus *bus = gst_element_get_bus(pipeline);
    DecodeEnd end = [&]() {
        if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            return DecodeEnd::Error;
        }
        GstMessage *message = gst_bus_timed_pop_filtered(
                bus, kExtractTimeout,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message == nullptr) {
            return DecodeEnd::Timeout;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *error = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            if (error != nullptr) {
                detail = error->message;
            }
            g_clear_error(&error);
            g_free(debug);
            gst_message_unref(message);
            return DecodeEnd::Error;
        }
        gst_message_unref(message);
        return DecodeEnd::Eos;
    }();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return end;
}

} // namespace

AudioExtractResult extract_audio(const AudioExtractRequest &request)
{
    AudioExtractResult result;

    // Probe first: it says whether there is audio at all and how many streams,
    // so the index is validated and the no-audio case fails before the
    // destination is touched. It also separates a missing/unreadable source
    // (open/decode failure) from a source that opens but has no sound.
    const std::optional<NewMedia> media = probe(request.source);
    if (!media) {
        result.error = AudioExtractError::OpenOrDecode;
        result.message = "cannot open or probe source: " + request.source.string();
        return result;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(media->audio_tracks.size());
    if (count == 0 || request.stream_index >= count) {
        result.error = AudioExtractError::NoAudioStream;
        result.message = count == 0 ? "no audio stream in source: " + request.source.string()
                                    : "audio stream " + std::to_string(request.stream_index)
                        + " out of range (" + std::to_string(count) + " streams)";
        return result;
    }

    // Make the destination's directory and clear any stale file, so filesink
    // never fails on a missing parent and a failed run cannot leave a previous
    // extraction behind.
    std::error_code ec;
    const std::filesystem::path parent = request.destination.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            result.error = AudioExtractError::PipelineError;
            result.message = "cannot create destination directory: " + parent.string();
            return result;
        }
    }
    std::filesystem::remove(request.destination, ec);

    std::string detail;
    const DecodeEnd end =
            decode_to_pcm(request.source, request.destination, request.stream_index, detail);
    if (end != DecodeEnd::Eos) {
        std::filesystem::remove(request.destination, ec);
        result.error = AudioExtractError::PipelineError;
        result.message = end == DecodeEnd::Timeout
                ? "decode pipeline timed out"
                : (detail.empty() ? "decode pipeline failed" : detail);
        return result;
    }

    // A clean EOS must still have written samples: a decoder that reached the
    // end without producing any PCM is a failure, not an empty success.
    const std::uintmax_t bytes = std::filesystem::file_size(request.destination, ec);
    if (ec || bytes == 0 || bytes % sizeof(float) != 0) {
        std::filesystem::remove(request.destination, ec);
        result.error = AudioExtractError::PipelineError;
        result.message = "decode produced no PCM";
        return result;
    }
    result.samples = bytes / sizeof(float);
    result.duration_seconds =
            static_cast<double>(result.samples) / static_cast<double>(kSampleRate);
    return result;
}

} // namespace genesis::adapters::engine::ges
