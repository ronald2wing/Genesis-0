// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "adapters/engine/ges/media/Probe.h"

#include <cstdint>
#include <string>

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include "project/command/MediaCommands.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"

namespace genesis::adapters::engine::ges {

namespace {

using genesis::core::Rational;
using genesis::project::AudioTrack;
using genesis::project::MediaKind;
using genesis::project::NewMedia;

// Long enough for a local file, short enough that a missing plugin cannot hang
// an import forever.
constexpr GstClockTime kDiscoverTimeout = 5 * GST_SECOND;

// The codec name from a stream's caps: the structure's subtype, with the "x-"
// GStreamer puts on a codec stripped, so "video/x-vp8" reads "vp8" and
// "audio/x-vorbis" reads "vorbis" - the short name the host shows.
std::string codec_name(GstDiscovererStreamInfo *stream)
{
    GstCaps *caps = gst_discoverer_stream_info_get_caps(stream);
    if (caps == nullptr || gst_caps_is_empty(caps)) {
        return { };
    }
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    if (structure == nullptr) {
        return { };
    }
    const gchar *name = gst_structure_get_name(structure);
    if (name == nullptr) {
        return { };
    }
    std::string subtype(name);
    const std::size_t slash = subtype.find_last_of('/');
    if (slash != std::string::npos) {
        subtype = subtype.substr(slash + 1);
    }
    if (subtype.size() > 2 && subtype[0] == 'x' && subtype[1] == '-') {
        subtype.erase(0, 2);
    }
    return subtype;
}

// A metadata tag on a stream, when it carries one; empty otherwise.
std::string stream_tag(GstDiscovererStreamInfo *stream, const gchar *tag)
{
    const GstTagList *tags = gst_discoverer_stream_info_get_tags(stream);
    if (tags == nullptr) {
        return { };
    }
    gchar *value = nullptr;
    if (!gst_tag_list_get_string(tags, tag, &value) || value == nullptr) {
        return { };
    }
    std::string out(value);
    g_free(value);
    return out;
}

// One in-flight async discovery, completed on a GMainLoop. The sync
// discover_uri completes inline for a local file; the async form is the
// fallback when it comes back null - the same loop pattern the GES asset
// request needs, because sync calls return null by design in some cases.
struct DiscoverRequest
{
    GMainLoop *loop = nullptr;
    GstDiscovererInfo *info = nullptr;
    GError *error = nullptr;
};

void on_discovered(GstDiscoverer *, GstDiscovererInfo *info, const GError *error,
                   gpointer user_data)
{
    auto *request = static_cast<DiscoverRequest *>(user_data);
    // A failure can arrive with both an info and an error; the error wins. On
    // success the info the signal hands over is borrowed, so take our own copy
    // that outlives the discoverer, which is stopped and freed below.
    if (error != nullptr) {
        request->error = g_error_copy(error);
    } else if (info != nullptr) {
        request->info = gst_discoverer_info_copy(info);
    }
    g_main_loop_quit(request->loop);
}

// Discovers `uri`, sync first and async over a GMainLoop as the fallback. The
// caller owns the returned info (gst_discoverer_info_unref).
GstDiscovererInfo *discover(const gchar *uri)
{
    GError *error = nullptr;
    GstDiscoverer *discoverer = gst_discoverer_new(kDiscoverTimeout, &error);
    if (discoverer == nullptr) {
        g_clear_error(&error);
        return nullptr;
    }

    // Sync first: the discoverer is not an asset request, so a local file
    // completes inline and this is the common path. A failure can come back as
    // a non-null info *with* a GError, so both must be read.
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri, &error);
    if (error == nullptr && info != nullptr) {
        g_object_unref(discoverer);
        return info;
    }
    if (info != nullptr) {
        gst_discoverer_info_unref(info);
    }
    g_clear_error(&error);
    g_object_unref(discoverer);

    // Async fallback. GStreamer's discover_uri_async takes no callback; the
    // result arrives on the "discovered" signal, so the loop blocks until it
    // fires or the discoverer's own timeout lapses.
    discoverer = gst_discoverer_new(kDiscoverTimeout, &error);
    if (discoverer == nullptr) {
        g_clear_error(&error);
        return nullptr;
    }
    DiscoverRequest request;
    request.loop = g_main_loop_new(nullptr, FALSE);
    g_signal_connect(discoverer, "discovered", G_CALLBACK(on_discovered), &request);
    gst_discoverer_start(discoverer);
    const gboolean started = gst_discoverer_discover_uri_async(discoverer, uri);
    if (started) {
        g_main_loop_run(request.loop);
    }
    gst_discoverer_stop(discoverer);
    g_main_loop_unref(request.loop);
    g_object_unref(discoverer);

    if (request.error != nullptr) {
        if (request.info != nullptr) {
            gst_discoverer_info_unref(request.info);
        }
        g_error_free(request.error);
        return nullptr;
    }
    return request.info;
}

// Fills the host's description from the discovered video and audio streams.
void fill_streams(NewMedia &out, GstDiscovererInfo *info)
{
    bool has_motion = false;
    bool has_image = false;
    bool filled_video = false;

    GList *video_streams = gst_discoverer_info_get_video_streams(info);
    for (GList *it = video_streams; it != nullptr; it = it->next) {
        auto *video = static_cast<GstDiscovererVideoInfo *>(it->data);
        if (gst_discoverer_video_info_is_image(video)) {
            has_image = true;
        } else {
            has_motion = true;
        }
        if (filled_video) {
            continue;
        }
        // The first video stream is the one the host describes.
        const guint width = gst_discoverer_video_info_get_width(video);
        const guint height = gst_discoverer_video_info_get_height(video);
        if (width > 0 && height > 0) {
            out.width = width;
            out.height = height;
        }
        const guint num = gst_discoverer_video_info_get_framerate_num(video);
        const guint denom = gst_discoverer_video_info_get_framerate_denom(video);
        if (num > 0 && denom > 0) {
            // The exact fraction stays exact; the double is for display.
            out.frame_rate = static_cast<double>(num) / static_cast<double>(denom);
            out.frame_rate_fraction = std::to_string(num) + "/" + std::to_string(denom);
        }
        const std::string codec = codec_name(GST_DISCOVERER_STREAM_INFO(video));
        if (!codec.empty()) {
            out.video_codec = codec;
        }
        filled_video = true;
    }

    // A moving picture is Video; a still is Image; nothing visual is Audio.
    if (has_motion) {
        out.kind = MediaKind::Video;
    } else if (has_image) {
        out.kind = MediaKind::Image;
    } else {
        out.kind = MediaKind::Audio;
    }

    GList *audio_streams = gst_discoverer_info_get_audio_streams(info);
    out.has_audio = audio_streams != nullptr;
    std::uint32_t index = 0;
    for (GList *it = audio_streams; it != nullptr; it = it->next) {
        auto *audio = static_cast<GstDiscovererAudioInfo *>(it->data);
        AudioTrack track;
        // GStreamer's discoverer names a stream by an id string, not a numeric
        // index; the position in file order is what Clip::audio_stream holds.
        track.index = index++;
        track.codec = codec_name(GST_DISCOVERER_STREAM_INFO(audio));
        track.channels = gst_discoverer_audio_info_get_channels(audio);
        track.sample_rate = gst_discoverer_audio_info_get_sample_rate(audio);
        track.title = stream_tag(GST_DISCOVERER_STREAM_INFO(audio), GST_TAG_TITLE);
        const gchar *language = gst_discoverer_audio_info_get_language(audio);
        if (language != nullptr) {
            track.language = language;
        }
        if (!out.audio_codec && !track.codec.empty()) {
            out.audio_codec = track.codec;
        }
        out.audio_tracks.push_back(std::move(track));
    }
}

} // namespace

std::optional<NewMedia> probe(const std::filesystem::path &path)
{
    // The discoverer only takes URIs; a bare path would read as a relative URI
    // and never resolve to the file.
    gchar *uri = gst_filename_to_uri(path.string().c_str(), nullptr);
    if (uri == nullptr) {
        return std::nullopt;
    }
    GstDiscovererInfo *info = discover(uri);
    g_free(uri);
    if (info == nullptr) {
        return std::nullopt;
    }

    NewMedia media;
    media.path = std::filesystem::absolute(path).string();
    media.name = path.stem().string();

    // The discoverer reports nanoseconds; the host stores seconds, approximated
    // through the documented seam rather than round-tripped as a double.
    const GstClockTime duration_ns = gst_discoverer_info_get_duration(info);
    if (duration_ns != GST_CLOCK_TIME_NONE) {
        media.duration = Rational::approximate(static_cast<double>(duration_ns)
                                               / static_cast<double>(GST_SECOND));
    }

    fill_streams(media, info);
    gst_discoverer_info_unref(info);
    return media;
}

} // namespace genesis::adapters::engine::ges
