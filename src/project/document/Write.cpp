// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The document's struct writers: model values to their on-disk JSON.

#include "project/Document.h"
#include "project/document/Internal.h"
#include "project/document/Json.h"

namespace genesis::project::detail {

json write_audio_track(const AudioTrack &track)
{
    json j = json::object();
    j["index"] = track.index;
    j["codec"] = track.codec;
    j["channels"] = track.channels;
    j["sampleRate"] = track.sample_rate;
    // Empty title/language are the absent values, left out.
    if (!track.title.empty()) {
        j["title"] = track.title;
    }
    if (!track.language.empty()) {
        j["language"] = track.language;
    }
    return j;
}

json write_media_item(const MediaItem &item)
{
    json j = json::object();
    j["id"] = item.id;
    j["path"] = item.path;
    j["name"] = item.name;
    // Unlike the other optionals, `duration` has no skip rule, so
    // it is written as null rather than omitted when the container said none.
    j["duration"] = item.duration ? json(encode_time(*item.duration)) : json(nullptr);
    j["kind"] = media_kind_name(item.kind);
    j["width"] = item.width ? json(*item.width) : json(nullptr);
    j["height"] = item.height ? json(*item.height) : json(nullptr);
    j["frameRate"] = item.frame_rate ? json(*item.frame_rate) : json(nullptr);
    j["frameRateFraction"] =
            item.frame_rate_fraction ? json(*item.frame_rate_fraction) : json(nullptr);
    j["videoCodec"] = item.video_codec ? json(*item.video_codec) : json(nullptr);
    j["audioCodec"] = item.audio_codec ? json(*item.audio_codec) : json(nullptr);
    j["hasAudio"] = item.has_audio;
    if (!item.audio_tracks.empty()) {
        json tracks = json::array();
        for (const AudioTrack &track : item.audio_tracks) {
            tracks.push_back(write_audio_track(track));
        }
        j["audioTracks"] = std::move(tracks);
    }
    if (item.placeholder) {
        j["placeholder"] = true;
    }
    if (item.color_range) {
        j["colorRange"] = color_range_name(*item.color_range);
    }
    if (item.color_space != ColorSpace::Sdr) {
        j["colorSpace"] = color_space_name(item.color_space);
    }
    if (item.origin) {
        j["origin"] = media_origin_name(*item.origin);
    }
    return j;
}

json write_track(const Track &track)
{
    json j = json::object();
    j["id"] = track.id;
    j["visible"] = track.visible;
    j["muted"] = track.muted;
    return j;
}

json write_param_key(const ParamKey &key)
{
    json j = json::object();
    j["at"] = key.at;
    j["value"] = key.value;
    j["ease"] = write_ease(key.ease);
    return j;
}

json write_applied_filter(const AppliedFilter &filter)
{
    json j = json::object();
    j["id"] = filter.id;
    json params = json::object();
    for (const auto &[name, value] : filter.params) {
        params[name] = value;
    }
    j["params"] = std::move(params);
    j["enabled"] = filter.enabled;
    if (!filter.keys.empty()) {
        json runs = json::object();
        for (const auto &[name, run] : filter.keys) {
            json keys = json::array();
            for (const ParamKey &key : run) {
                keys.push_back(write_param_key(key));
            }
            runs[name] = std::move(keys);
        }
        j["keys"] = std::move(runs);
    }
    return j;
}

json write_filter_chain(const std::vector<AppliedFilter> &chain)
{
    json filters = json::array();
    for (const AppliedFilter &filter : chain) {
        filters.push_back(write_applied_filter(filter));
    }
    return filters;
}

json write_crop(const Crop &crop)
{
    json j = json::object();
    j["left"] = crop.left;
    j["top"] = crop.top;
    j["right"] = crop.right;
    j["bottom"] = crop.bottom;
    return j;
}

json write_stroke(const Stroke &stroke)
{
    json j = json::object();
    j["tool"] = brush_tool_name(stroke.tool);
    j["size"] = stroke.size;
    json points = json::array();
    for (const std::array<double, 2> &point : stroke.points) {
        points.push_back(json::array({ point[0], point[1] }));
    }
    j["points"] = std::move(points);
    if (stroke.at) {
        j["at"] = encode_time(*stroke.at);
    }
    return j;
}

json write_cutout(const Cutout &cutout)
{
    json j = json::object();
    j["mode"] = cutout_mode_name(cutout.mode);
    if (!is_auto(cutout.subject)) {
        j["subject"] = std::string(key(cutout.subject));
    }
    j["feather"] = cutout.feather;
    if (!cutout.strokes.empty()) {
        json strokes = json::array();
        for (const Stroke &stroke : cutout.strokes) {
            strokes.push_back(write_stroke(stroke));
        }
        j["strokes"] = std::move(strokes);
    }
    return j;
}

json write_clip_key(const ClipKey &key)
{
    json j = json::object();
    j["property"] = std::string(name(key.property));
    j["at"] = key.at;
    j["value"] = key.value;
    j["ease"] = write_ease(key.ease);
    return j;
}

json write_speed_point(const SpeedPoint &point)
{
    json j = json::object();
    j["at"] = point.at;
    j["speed"] = point.speed;
    return j;
}

json write_transition(const Transition &transition)
{
    json j = json::object();
    j["id"] = transition.id;
    j["duration"] = encode_time(transition.duration);
    return j;
}

json write_text_style(const TextStyle &style)
{
    json j = json::object();
    j["content"] = style.content;
    j["fontFamily"] = style.font_family;
    j["fontSize"] = style.font_size;
    j["fontWeight"] = style.font_weight;
    j["italic"] = style.italic;
    j["color"] = style.color;
    j["align"] = text_align_name(style.align);
    j["opacity"] = style.opacity;
    j["strokeWidth"] = style.stroke_width;
    j["strokeColor"] = style.stroke_color;
    j["shadow"] = style.shadow;
    j["background"] = style.background;
    j["backgroundRadius"] = style.background_radius;
    j["backgroundPaddingX"] = style.background_padding_x;
    j["backgroundPaddingY"] = style.background_padding_y;
    j["lineHeight"] = style.line_height;
    j["tracking"] = style.tracking;
    j["maxWidth"] = style.max_width;
    j["maxHeight"] = style.max_height;
    return j;
}

json write_clip(const Clip &clip)
{
    json j = json::object();
    j["id"] = clip.id;
    j["trackId"] = clip.track_id;
    j["mediaId"] = clip.media_id;
    j["name"] = clip.name;
    j["kind"] = clip_kind_name(clip.kind);
    j["start"] = encode_time(clip.start);
    j["duration"] = encode_time(clip.duration);
    j["sourceStart"] = encode_time(clip.source_start);
    j["volume"] = clip.volume;
    // Pan at centre is the absent value, left out.
    if (clip.pan != 0.0) {
        j["pan"] = clip.pan;
    }
    j["fadeIn"] = encode_time(clip.fade_in);
    j["fadeOut"] = encode_time(clip.fade_out);
    j["scale"] = clip.scale;
    j["offsetX"] = clip.offset_x;
    j["offsetY"] = clip.offset_y;
    j["rotation"] = clip.rotation;
    // Stretch at unity is the absent value, left out.
    if (clip.stretch_x != 1.0) {
        j["stretchX"] = clip.stretch_x;
    }
    if (clip.stretch_y != 1.0) {
        j["stretchY"] = clip.stretch_y;
    }
    j["opacity"] = clip.opacity;
    j["speed"] = clip.speed;
    if (clip.speed_curve) {
        json curve = json::array();
        for (const SpeedPoint &point : *clip.speed_curve) {
            curve.push_back(write_speed_point(point));
        }
        j["speedCurve"] = std::move(curve);
    }
    if (!clip.keys.empty()) {
        json keys = json::array();
        for (const ClipKey &key : clip.keys) {
            keys.push_back(write_clip_key(key));
        }
        j["keys"] = std::move(keys);
    }
    if (clip.flip_h) {
        j["flipH"] = true;
    }
    if (clip.flip_v) {
        j["flipV"] = true;
    }
    if (clip.reverse) {
        j["reverse"] = true;
    }
    if (!clip.blend.empty()) {
        j["blend"] = clip.blend;
    }
    if (clip.crop) {
        j["crop"] = write_crop(*clip.crop);
    }
    if (clip.cutout) {
        j["cutout"] = write_cutout(*clip.cutout);
    }
    j["preservePitch"] = clip.preserve_pitch;
    // Chains are always written, even empty.
    j["filters"] = write_filter_chain(clip.filters);
    j["videoEffects"] = write_filter_chain(clip.video_effects);
    if (clip.audio_stream) {
        j["audioStream"] = *clip.audio_stream;
    }
    if (clip.muted) {
        j["muted"] = *clip.muted;
    }
    if (clip.detached_from) {
        j["detachedFrom"] = *clip.detached_from;
    }
    if (clip.transition_in) {
        j["transitionIn"] = write_transition(*clip.transition_in);
    }
    if (clip.text) {
        j["text"] = write_text_style(*clip.text);
    }
    return j;
}

json write_video_settings(const VideoSettings &video)
{
    json j = json::object();
    j["width"] = video.width;
    j["height"] = video.height;
    j["rateNum"] = video.rate_num;
    j["rateDen"] = video.rate_den;
    if (video.color_space != ColorSpace::Sdr) {
        j["colorSpace"] = color_space_name(video.color_space);
    }
    return j;
}

json write_timeline(const Timeline &timeline)
{
    json j = json::object();
    j["id"] = timeline.id;
    j["name"] = timeline.name;
    j["video"] = write_video_settings(timeline.video);
    json tracks = json::array();
    for (const Track &track : timeline.tracks) {
        tracks.push_back(write_track(track));
    }
    j["tracks"] = std::move(tracks);
    json clips = json::array();
    for (const Clip &clip : timeline.clips) {
        clips.push_back(write_clip(clip));
    }
    j["clips"] = std::move(clips);
    return j;
}

json write_font(const CustomFont &font)
{
    json j = json::object();
    j["family"] = font.family;
    j["path"] = font.path;
    return j;
}

} // namespace genesis::project::detail
