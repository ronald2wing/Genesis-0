// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The pad-added plumbing every decode pipeline shares. uridecodebin exposes one
// pad per stream, and a pad left unlinked stalls or posts a not-linked error,
// so each consumer has to answer every pad: wire it to its sink, or consume it
// with a fakesink.

#pragma once

#include <cstdint>

#include <gst/gst.h>

namespace genesis::adapters::engine::ges {

enum class PadMedia { Other, Audio, Video };

// The media kind of a decodebin pad, from its current caps or, before the first
// buffer negotiates, its possible caps.
inline PadMedia pad_media(GstPad *pad)
{
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    if (caps == nullptr) {
        return PadMedia::Other;
    }
    const GstStructure *structure = gst_caps_get_structure(caps, 0);
    const gchar *name = structure == nullptr ? nullptr : gst_structure_get_name(structure);
    PadMedia media = PadMedia::Other;
    if (name != nullptr) {
        if (g_str_has_prefix(name, "video/")) {
            media = PadMedia::Video;
        } else if (g_str_has_prefix(name, "audio/")) {
            media = PadMedia::Audio;
        }
    }
    gst_caps_unref(caps);
    return media;
}

inline bool pad_is_video(GstPad *pad)
{
    return pad_media(pad) == PadMedia::Video;
}

inline bool pad_is_audio(GstPad *pad)
{
    return pad_media(pad) == PadMedia::Audio;
}

// Wires the first video pad uridecodebin exposes to `convert`'s sink; every
// other pad is left unlinked, its buffers dropped by the source. Used by the
// picture-only consumers (filmstrip, proxy, reverse).
inline void link_video_pad(GstElement *, GstPad *pad, gpointer user_data)
{
    if (!pad_is_video(pad)) {
        return;
    }
    auto *convert = static_cast<GstElement *>(user_data);
    GstPad *sink = gst_element_get_static_pad(convert, "sink");
    if (sink == nullptr) {
        return;
    }
    if (!gst_pad_is_linked(sink)) {
        gst_pad_link(pad, sink);
    }
    gst_object_unref(sink);
}

// The state the target-pad handler drives: it keeps the chosen stream (the
// target-th one, in file order) and routes every other stream to a fresh
// fakesink, so no pad is left unlinked.
struct PadSelect
{
    GstElement *pipeline = nullptr;
    GstElement *convert = nullptr;
    std::uint32_t target = 0; // which stream (file order) to keep
    std::uint32_t seen = 0; // streams of `wanted` met so far
    bool linked = false; // the target pad is already wired
    PadMedia wanted = PadMedia::Other;
};

inline void on_target_pad_added(GstElement *, GstPad *pad, gpointer user_data)
{
    auto *select = static_cast<PadSelect *>(user_data);

    const bool matches = pad_media(pad) == select->wanted;
    if (matches && select->seen == select->target && !select->linked) {
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
    if (matches) {
        ++select->seen;
    }

    // Any other stream (video, subtitles, an unselected track) is consumed by
    // its own fakesink so it cannot stall the decode.
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

} // namespace genesis::adapters::engine::ges
