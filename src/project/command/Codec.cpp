// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The command codec: every Command to and from its tagged-`op` JSON.
//
// Encode mirrors the `op` + camelCase field spelling, with the
// one host divergence that every Rational time is written as the exact
// "num/den" string rather than an f64.
//
// Decode is fully strict. Unlike the document layer, which drops a malformed
// list entry so a hand-edited file still opens, a command is a programmatic
// message: any malformed element (a wrong-typed scalar, an unknown enum
// spelling, a bad ease, a bad nested or list entry) rejects the whole command.
// There is no "degrade gracefully" contract for a message.

#include "project/command/Codec.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "project/Document.h"
#include "project/document/Json.h"

namespace genesis::project::command {

namespace {

using nlohmann::json;

// -- enum spellings (the two that have no document mirror) ------------------

const char *trim_edge_name(TrimEdge edge)
{
    switch (edge) {
    case TrimEdge::Start:
        return "start";
    case TrimEdge::End:
        return "end";
    }
    return "start";
}

const char *track_flag_name(TrackFlag flag)
{
    switch (flag) {
    case TrackFlag::Visible:
        return "visible";
    case TrackFlag::Muted:
        return "muted";
    }
    return "visible";
}

// -- encoders ---------------------------------------------------------------

json encode_audio_track(const AudioTrack &track)
{
    json j = json::object();
    j["index"] = track.index;
    j["codec"] = track.codec;
    j["channels"] = track.channels;
    j["sampleRate"] = track.sample_rate;
    if (!track.title.empty()) {
        j["title"] = track.title;
    }
    if (!track.language.empty()) {
        j["language"] = track.language;
    }
    return j;
}

json encode_new_media(const NewMedia &item)
{
    json j = json::object();
    j["path"] = item.path;
    j["name"] = item.name;
    j["duration"] = item.duration ? json(encode_time(*item.duration)) : json(nullptr);
    j["kind"] = detail::media_kind_name(item.kind);
    j["width"] = item.width ? json(*item.width) : json(nullptr);
    j["height"] = item.height ? json(*item.height) : json(nullptr);
    j["frameRate"] = item.frame_rate ? json(*item.frame_rate) : json(nullptr);
    j["frameRateFraction"] =
            item.frame_rate_fraction ? json(*item.frame_rate_fraction) : json(nullptr);
    j["videoCodec"] = item.video_codec ? json(*item.video_codec) : json(nullptr);
    j["audioCodec"] = item.audio_codec ? json(*item.audio_codec) : json(nullptr);
    j["hasAudio"] = item.has_audio;
    json tracks = json::array();
    for (const AudioTrack &track : item.audio_tracks) {
        tracks.push_back(encode_audio_track(track));
    }
    j["audioTracks"] = std::move(tracks);
    j["origin"] = item.origin ? json(std::string(detail::media_origin_name(*item.origin)))
                              : json(nullptr);
    j["colorSpace"] = detail::color_space_name(item.color_space);
    return j;
}

json encode_param_key(const ParamKey &key)
{
    json j = json::object();
    j["at"] = key.at;
    j["value"] = key.value;
    j["ease"] = detail::write_ease(key.ease);
    return j;
}

json encode_applied_filter(const AppliedFilter &filter)
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
                keys.push_back(encode_param_key(key));
            }
            runs[name] = std::move(keys);
        }
        j["keys"] = std::move(runs);
    }
    return j;
}

json encode_filter_chain(const std::vector<AppliedFilter> &chain)
{
    json filters = json::array();
    for (const AppliedFilter &filter : chain) {
        filters.push_back(encode_applied_filter(filter));
    }
    return filters;
}

json encode_crop(const Crop &crop)
{
    json j = json::object();
    j["left"] = crop.left;
    j["top"] = crop.top;
    j["right"] = crop.right;
    j["bottom"] = crop.bottom;
    return j;
}

json encode_stroke(const Stroke &stroke)
{
    json j = json::object();
    j["tool"] = detail::brush_tool_name(stroke.tool);
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

json encode_cutout(const Cutout &cutout)
{
    json j = json::object();
    j["mode"] = detail::cutout_mode_name(cutout.mode);
    if (!is_auto(cutout.subject)) {
        j["subject"] = std::string(key(cutout.subject));
    }
    j["feather"] = cutout.feather;
    if (!cutout.strokes.empty()) {
        json strokes = json::array();
        for (const Stroke &stroke : cutout.strokes) {
            strokes.push_back(encode_stroke(stroke));
        }
        j["strokes"] = std::move(strokes);
    }
    return j;
}

json encode_speed_point(const SpeedPoint &point)
{
    json j = json::object();
    j["at"] = point.at;
    j["speed"] = point.speed;
    return j;
}

json encode_speed_curve(const std::vector<SpeedPoint> &curve)
{
    json points = json::array();
    for (const SpeedPoint &point : curve) {
        points.push_back(encode_speed_point(point));
    }
    return points;
}

json encode_transition(const Transition &transition)
{
    json j = json::object();
    j["id"] = transition.id;
    j["duration"] = encode_time(transition.duration);
    return j;
}

json encode_text_style(const TextStyle &style)
{
    json j = json::object();
    j["content"] = style.content;
    j["fontFamily"] = style.font_family;
    j["fontSize"] = style.font_size;
    j["fontWeight"] = style.font_weight;
    j["italic"] = style.italic;
    j["color"] = style.color;
    j["align"] = detail::text_align_name(style.align);
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

json encode_clip_move(const ClipMove &move)
{
    return json{ { "clipId", move.clip_id },
                 { "start", encode_time(move.start) },
                 { "trackId", move.track_id } };
}

json encode_video_settings(const VideoSettings &video)
{
    json j = json::object();
    j["width"] = video.width;
    j["height"] = video.height;
    j["rateNum"] = video.rate_num;
    j["rateDen"] = video.rate_den;
    if (video.color_space != ColorSpace::Sdr) {
        j["colorSpace"] = detail::color_space_name(video.color_space);
    }
    return j;
}

json encode_clip_patch(const ClipPatch &patch)
{
    json j = json::object();
    j["name"] = patch.name ? json(*patch.name) : json(nullptr);
    j["volume"] = patch.volume ? json(*patch.volume) : json(nullptr);
    j["fadeIn"] = patch.fade_in ? json(encode_time(*patch.fade_in)) : json(nullptr);
    j["fadeOut"] = patch.fade_out ? json(encode_time(*patch.fade_out)) : json(nullptr);
    j["opacity"] = patch.opacity ? json(*patch.opacity) : json(nullptr);
    j["preservePitch"] = patch.preserve_pitch ? json(*patch.preserve_pitch) : json(nullptr);
    j["muted"] = patch.muted ? json(*patch.muted) : json(nullptr);
    j["flipH"] = patch.flip_h ? json(*patch.flip_h) : json(nullptr);
    j["flipV"] = patch.flip_v ? json(*patch.flip_v) : json(nullptr);
    j["blend"] = patch.blend ? json(*patch.blend) : json(nullptr);
    if (patch.crop) {
        j["crop"] = *patch.crop ? encode_crop(**patch.crop) : json(nullptr);
    }
    j["filters"] = patch.filters ? encode_filter_chain(*patch.filters) : json(nullptr);
    j["videoEffects"] =
            patch.video_effects ? encode_filter_chain(*patch.video_effects) : json(nullptr);
    if (patch.transition_in) {
        j["transitionIn"] =
                *patch.transition_in ? encode_transition(**patch.transition_in) : json(nullptr);
    }
    if (patch.text) {
        j["text"] = *patch.text ? encode_text_style(**patch.text) : json(nullptr);
    }
    if (patch.audio_stream) {
        j["audioStream"] = *patch.audio_stream ? json(**patch.audio_stream) : json(nullptr);
    }
    return j;
}

// -- strict enum readers ----------------------------------------------------

std::optional<MediaKind> read_media_kind_strict(const json &value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string s = value.get<std::string>();
    if (s == "video") {
        return MediaKind::Video;
    }
    if (s == "audio") {
        return MediaKind::Audio;
    }
    if (s == "image") {
        return MediaKind::Image;
    }
    return std::nullopt;
}

std::optional<ColorSpace> read_color_space_strict(const json &value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string s = value.get<std::string>();
    if (s == "sdr") {
        return ColorSpace::Sdr;
    }
    if (s == "hlg") {
        return ColorSpace::Hlg;
    }
    if (s == "pq") {
        return ColorSpace::Pq;
    }
    return std::nullopt;
}

std::optional<TextAlign> read_text_align_strict(const json &value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string s = value.get<std::string>();
    if (s == "left") {
        return TextAlign::Left;
    }
    if (s == "center") {
        return TextAlign::Center;
    }
    if (s == "right") {
        return TextAlign::Right;
    }
    return std::nullopt;
}

std::optional<TrimEdge> read_trim_edge(const json &value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string s = value.get<std::string>();
    if (s == "start") {
        return TrimEdge::Start;
    }
    if (s == "end") {
        return TrimEdge::End;
    }
    return std::nullopt;
}

std::optional<TrackFlag> read_track_flag(const json &value)
{
    if (!value.is_string()) {
        return std::nullopt;
    }
    const std::string s = value.get<std::string>();
    if (s == "visible") {
        return TrackFlag::Visible;
    }
    if (s == "muted") {
        return TrackFlag::Muted;
    }
    return std::nullopt;
}

// A preset name ("in"/"out"/"inOut") or an array of exactly four finite
// numbers. Anything else - including the document layer's tolerant fallback to
// linear - rejects: a command naming "linear" or a malformed curve is a bug,
// not a hand-edit.
std::optional<KeyEase> read_ease_strict(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "in") {
            return KeyEase::in();
        }
        if (s == "out") {
            return KeyEase::out();
        }
        if (s == "inOut") {
            return KeyEase::in_out();
        }
        return std::nullopt;
    }
    if (value.is_array() && value.size() == 4) {
        std::array<double, 4> points{ };
        for (std::size_t i = 0; i < 4; ++i) {
            if (!value[i].is_number()) {
                return std::nullopt;
            }
            points[i] = value[i].get<double>();
            if (!std::isfinite(points[i])) {
                return std::nullopt;
            }
        }
        return KeyEase{ points };
    }
    return std::nullopt;
}

// -- scalar helper ----------------------------------------------------------

// A std::size_t field (an entry or tab index) is required and must be a
// non-negative JSON integer. There is no take_size in the document layer.
bool read_size(const json &value, std::size_t &out)
{
    if (!value.is_number_unsigned()) {
        return false;
    }
    out = value.get<std::size_t>();
    return true;
}

// -- nested readers ---------------------------------------------------------

std::optional<AudioTrack> read_audio_track(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    AudioTrack track{ };
    const auto index = j.find("index");
    if (index == j.end() || !detail::as_u32(*index, track.index)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "codec", track.codec)) {
        return std::nullopt;
    }
    if (!detail::take_u32(j, "channels", track.channels)) {
        return std::nullopt;
    }
    if (!detail::take_u32(j, "sampleRate", track.sample_rate)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "title", track.title)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "language", track.language)) {
        return std::nullopt;
    }
    return track;
}

std::optional<NewMedia> read_new_media(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    NewMedia item{ };
    if (!detail::take_string(j, "path", item.path)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "name", item.name)) {
        return std::nullopt;
    }
    if (!detail::take_opt_time(j, "duration", item.duration)) {
        return std::nullopt;
    }
    if (const auto it = j.find("kind"); it != j.end()) {
        const std::optional<MediaKind> kind = read_media_kind_strict(*it);
        if (!kind) {
            return std::nullopt;
        }
        item.kind = *kind;
    }
    if (!detail::take_opt_u32(j, "width", item.width)) {
        return std::nullopt;
    }
    if (!detail::take_opt_u32(j, "height", item.height)) {
        return std::nullopt;
    }
    if (!detail::take_opt_double(j, "frameRate", item.frame_rate)) {
        return std::nullopt;
    }
    if (!detail::take_opt_string(j, "frameRateFraction", item.frame_rate_fraction)) {
        return std::nullopt;
    }
    if (!detail::take_opt_string(j, "videoCodec", item.video_codec)) {
        return std::nullopt;
    }
    if (!detail::take_opt_string(j, "audioCodec", item.audio_codec)) {
        return std::nullopt;
    }
    if (!detail::take_bool(j, "hasAudio", item.has_audio)) {
        return std::nullopt;
    }
    if (const auto it = j.find("audioTracks"); it != j.end()) {
        if (!it->is_array()) {
            return std::nullopt;
        }
        for (const json &entry : *it) {
            std::optional<AudioTrack> track = read_audio_track(entry);
            if (!track) {
                return std::nullopt;
            }
            item.audio_tracks.push_back(std::move(*track));
        }
    }
    if (const auto it = j.find("origin"); it != j.end() && !it->is_null()) {
        const std::optional<MediaOrigin> origin = detail::read_media_origin(*it);
        if (!origin) {
            return std::nullopt;
        }
        item.origin = *origin;
    }
    if (const auto it = j.find("colorSpace"); it != j.end()) {
        const std::optional<ColorSpace> space = read_color_space_strict(*it);
        if (!space) {
            return std::nullopt;
        }
        item.color_space = *space;
    }
    return item;
}

std::optional<ParamKey> read_param_key(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    ParamKey key{ };
    const auto at = j.find("at");
    if (at == j.end() || !detail::as_double(*at, key.at)) {
        return std::nullopt;
    }
    const auto value = j.find("value");
    if (value == j.end() || !detail::as_double(*value, key.value)) {
        return std::nullopt;
    }
    if (const auto it = j.find("ease"); it != j.end()) {
        const std::optional<KeyEase> ease = read_ease_strict(*it);
        if (!ease) {
            return std::nullopt;
        }
        key.ease = *ease;
    }
    return key;
}

std::optional<AppliedFilter> read_applied_filter(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    AppliedFilter filter{ };
    const auto id = j.find("id");
    if (id == j.end() || !detail::as_string(*id, filter.id)) {
        return std::nullopt;
    }
    if (const auto it = j.find("params"); it != j.end()) {
        if (!it->is_object()) {
            return std::nullopt;
        }
        for (const auto &[name, value] : it->items()) {
            if (!value.is_number()) {
                return std::nullopt;
            }
            filter.params[name] = value.get<double>();
        }
    }
    if (!detail::take_bool(j, "enabled", filter.enabled)) {
        return std::nullopt;
    }
    if (const auto it = j.find("keys"); it != j.end()) {
        if (!it->is_object()) {
            return std::nullopt;
        }
        for (const auto &[name, run] : it->items()) {
            if (!run.is_array()) {
                return std::nullopt;
            }
            std::vector<ParamKey> keys;
            for (const json &entry : run) {
                std::optional<ParamKey> key = read_param_key(entry);
                if (!key) {
                    return std::nullopt;
                }
                keys.push_back(std::move(*key));
            }
            filter.keys[name] = std::move(keys);
        }
    }
    return filter;
}

std::optional<std::vector<AppliedFilter>> read_filter_chain(const json &value)
{
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<AppliedFilter> chain;
    for (const json &entry : value) {
        std::optional<AppliedFilter> filter = read_applied_filter(entry);
        if (!filter) {
            return std::nullopt;
        }
        chain.push_back(std::move(*filter));
    }
    return chain;
}

std::optional<Crop> read_crop(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Crop crop{ };
    const auto left = j.find("left");
    if (left == j.end() || !detail::as_double(*left, crop.left)) {
        return std::nullopt;
    }
    const auto top = j.find("top");
    if (top == j.end() || !detail::as_double(*top, crop.top)) {
        return std::nullopt;
    }
    const auto right = j.find("right");
    if (right == j.end() || !detail::as_double(*right, crop.right)) {
        return std::nullopt;
    }
    const auto bottom = j.find("bottom");
    if (bottom == j.end() || !detail::as_double(*bottom, crop.bottom)) {
        return std::nullopt;
    }
    return crop;
}

std::optional<Stroke> read_stroke(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Stroke stroke{ };
    const auto tool = j.find("tool");
    if (tool == j.end()) {
        return std::nullopt;
    }
    if (const std::optional<BrushTool> t = detail::read_brush_tool(*tool)) {
        stroke.tool = *t;
    } else {
        return std::nullopt;
    }
    const auto size = j.find("size");
    if (size == j.end() || !detail::as_double(*size, stroke.size)) {
        return std::nullopt;
    }
    const auto points = j.find("points");
    if (points == j.end() || !points->is_array()) {
        return std::nullopt;
    }
    for (const json &point : *points) {
        if (!point.is_array() || point.size() != 2 || !point[0].is_number()
            || !point[1].is_number()) {
            return std::nullopt;
        }
        stroke.points.push_back(
                std::array<double, 2>{ point[0].get<double>(), point[1].get<double>() });
    }
    if (!detail::take_opt_time(j, "at", stroke.at)) {
        return std::nullopt;
    }
    return stroke;
}

std::optional<Cutout> read_cutout(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Cutout cutout{ };
    const auto mode = j.find("mode");
    if (mode == j.end()) {
        return std::nullopt;
    }
    if (const std::optional<CutoutMode> m = detail::read_cutout_mode(*mode)) {
        cutout.mode = *m;
    } else {
        return std::nullopt;
    }
    if (const auto it = j.find("subject"); it != j.end()) {
        if (const std::optional<Subject> s = detail::read_subject(*it)) {
            cutout.subject = *s;
        } else {
            return std::nullopt;
        }
    }
    if (!detail::take_double(j, "feather", cutout.feather)) {
        return std::nullopt;
    }
    if (const auto it = j.find("strokes"); it != j.end()) {
        if (!it->is_array()) {
            return std::nullopt;
        }
        for (const json &entry : *it) {
            std::optional<Stroke> stroke = read_stroke(entry);
            if (!stroke) {
                return std::nullopt;
            }
            cutout.strokes.push_back(std::move(*stroke));
        }
    }
    return cutout;
}

std::optional<SpeedPoint> read_speed_point(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    SpeedPoint point{ };
    const auto at = j.find("at");
    if (at == j.end() || !detail::as_double(*at, point.at)) {
        return std::nullopt;
    }
    const auto speed = j.find("speed");
    if (speed == j.end() || !detail::as_double(*speed, point.speed)) {
        return std::nullopt;
    }
    return point;
}

std::optional<std::vector<SpeedPoint>> read_speed_curve(const json &value)
{
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<SpeedPoint> curve;
    for (const json &entry : value) {
        std::optional<SpeedPoint> point = read_speed_point(entry);
        if (!point) {
            return std::nullopt;
        }
        curve.push_back(std::move(*point));
    }
    return curve;
}

std::optional<Transition> read_transition(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Transition transition{ };
    const auto id = j.find("id");
    if (id == j.end() || !detail::as_string(*id, transition.id)) {
        return std::nullopt;
    }
    if (!detail::take_time(j, "duration", transition.duration)) {
        return std::nullopt;
    }
    return transition;
}

std::optional<TextStyle> read_text_style(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    TextStyle style{ };
    if (!detail::take_string(j, "content", style.content)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "fontFamily", style.font_family)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "fontSize", style.font_size)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "fontWeight", style.font_weight)) {
        return std::nullopt;
    }
    if (!detail::take_bool(j, "italic", style.italic)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "color", style.color)) {
        return std::nullopt;
    }
    if (const auto it = j.find("align"); it != j.end()) {
        const std::optional<TextAlign> align = read_text_align_strict(*it);
        if (!align) {
            return std::nullopt;
        }
        style.align = *align;
    }
    if (!detail::take_double(j, "opacity", style.opacity)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "strokeWidth", style.stroke_width)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "strokeColor", style.stroke_color)) {
        return std::nullopt;
    }
    if (!detail::take_bool(j, "shadow", style.shadow)) {
        return std::nullopt;
    }
    if (!detail::take_string(j, "background", style.background)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "backgroundRadius", style.background_radius)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "backgroundPaddingX", style.background_padding_x)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "backgroundPaddingY", style.background_padding_y)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "lineHeight", style.line_height)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "tracking", style.tracking)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "maxWidth", style.max_width)) {
        return std::nullopt;
    }
    if (!detail::take_double(j, "maxHeight", style.max_height)) {
        return std::nullopt;
    }
    return style;
}

std::optional<ClipMove> read_clip_move(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    ClipMove move{ };
    const auto clip_id = j.find("clipId");
    if (clip_id == j.end() || !detail::as_string(*clip_id, move.clip_id)) {
        return std::nullopt;
    }
    const auto start = j.find("start");
    if (start == j.end()) {
        return std::nullopt;
    }
    const std::optional<Rational> t = decode_time(*start);
    if (!t) {
        return std::nullopt;
    }
    move.start = *t;
    const auto track_id = j.find("trackId");
    if (track_id == j.end() || !detail::as_string(*track_id, move.track_id)) {
        return std::nullopt;
    }
    return move;
}

std::optional<VideoSettings> read_video_settings(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    VideoSettings video{ };
    const auto width = j.find("width");
    if (width == j.end() || !detail::as_u32(*width, video.width)) {
        return std::nullopt;
    }
    const auto height = j.find("height");
    if (height == j.end() || !detail::as_u32(*height, video.height)) {
        return std::nullopt;
    }
    const auto rate_num = j.find("rateNum");
    if (rate_num == j.end() || !detail::as_i64(*rate_num, video.rate_num)) {
        return std::nullopt;
    }
    const auto rate_den = j.find("rateDen");
    if (rate_den == j.end() || !detail::as_i64(*rate_den, video.rate_den)) {
        return std::nullopt;
    }
    if (const auto it = j.find("colorSpace"); it != j.end()) {
        const std::optional<ColorSpace> space = read_color_space_strict(*it);
        if (!space) {
            return std::nullopt;
        }
        video.color_space = *space;
    }
    return video;
}

// The double-optionals: absent leaves the field untouched (outer nullopt),
// null clears it (inner nullopt), a value replaces it. The four are crop,
// transition_in, text and audio_stream.
std::optional<ClipPatch> read_clip_patch(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    ClipPatch patch{ };
    if (!detail::take_opt_string(j, "name", patch.name)) {
        return std::nullopt;
    }
    if (!detail::take_opt_double(j, "volume", patch.volume)) {
        return std::nullopt;
    }
    if (!detail::take_opt_time(j, "fadeIn", patch.fade_in)) {
        return std::nullopt;
    }
    if (!detail::take_opt_time(j, "fadeOut", patch.fade_out)) {
        return std::nullopt;
    }
    if (!detail::take_opt_double(j, "opacity", patch.opacity)) {
        return std::nullopt;
    }
    if (!detail::take_opt_bool(j, "preservePitch", patch.preserve_pitch)) {
        return std::nullopt;
    }
    if (!detail::take_opt_bool(j, "muted", patch.muted)) {
        return std::nullopt;
    }
    if (!detail::take_opt_bool(j, "flipH", patch.flip_h)) {
        return std::nullopt;
    }
    if (!detail::take_opt_bool(j, "flipV", patch.flip_v)) {
        return std::nullopt;
    }
    if (!detail::take_opt_string(j, "blend", patch.blend)) {
        return std::nullopt;
    }
    if (const auto it = j.find("crop"); it != j.end()) {
        if (it->is_null()) {
            patch.crop = std::optional<Crop>{ };
        } else {
            std::optional<Crop> crop = read_crop(*it);
            if (!crop) {
                return std::nullopt;
            }
            patch.crop = std::optional<Crop>{ std::move(*crop) };
        }
    }
    if (const auto it = j.find("filters"); it != j.end() && !it->is_null()) {
        std::optional<std::vector<AppliedFilter>> filters = read_filter_chain(*it);
        if (!filters) {
            return std::nullopt;
        }
        patch.filters = std::move(*filters);
    }
    if (const auto it = j.find("videoEffects"); it != j.end() && !it->is_null()) {
        std::optional<std::vector<AppliedFilter>> effects = read_filter_chain(*it);
        if (!effects) {
            return std::nullopt;
        }
        patch.video_effects = std::move(*effects);
    }
    if (const auto it = j.find("transitionIn"); it != j.end()) {
        if (it->is_null()) {
            patch.transition_in = std::optional<Transition>{ };
        } else {
            std::optional<Transition> transition = read_transition(*it);
            if (!transition) {
                return std::nullopt;
            }
            patch.transition_in = std::optional<Transition>{ std::move(*transition) };
        }
    }
    if (const auto it = j.find("text"); it != j.end()) {
        if (it->is_null()) {
            patch.text = std::optional<TextStyle>{ };
        } else {
            std::optional<TextStyle> style = read_text_style(*it);
            if (!style) {
                return std::nullopt;
            }
            patch.text = std::optional<TextStyle>{ std::move(*style) };
        }
    }
    if (const auto it = j.find("audioStream"); it != j.end()) {
        if (it->is_null()) {
            patch.audio_stream = std::optional<std::uint32_t>{ };
        } else {
            std::uint32_t stream = 0;
            if (!detail::as_u32(*it, stream)) {
                return std::nullopt;
            }
            patch.audio_stream = std::optional<std::uint32_t>{ stream };
        }
    }
    return patch;
}

std::optional<std::vector<std::string>> read_string_list(const json &value)
{
    if (!value.is_array()) {
        return std::nullopt;
    }
    std::vector<std::string> list;
    for (const json &entry : value) {
        if (!entry.is_string()) {
            return std::nullopt;
        }
        list.push_back(entry.get<std::string>());
    }
    return list;
}

// -- encoder visitor --------------------------------------------------------

struct Encoder
{
    json operator()(const AddMedia &cmd) const
    {
        return json{ { "op", "addMedia" }, { "item", encode_new_media(cmd.item) } };
    }
    json operator()(const RemoveMedia &cmd) const
    {
        return json{ { "op", "removeMedia" }, { "mediaId", cmd.media_id } };
    }
    json operator()(const SetMediaPlaceholder &cmd) const
    {
        return json{ { "op", "setMediaPlaceholder" },
                     { "mediaId", cmd.media_id },
                     { "placeholder", cmd.placeholder } };
    }
    json operator()(const FillSlot &cmd) const
    {
        return json{ { "op", "fillSlot" },
                     { "mediaId", cmd.media_id },
                     { "item", encode_new_media(cmd.item) } };
    }
    json operator()(const Batch &cmd) const
    {
        json commands = json::array();
        for (const Command &c : cmd.commands) {
            commands.push_back(encode(c));
        }
        return json{ { "op", "batch" }, { "commands", std::move(commands) } };
    }
    json operator()(const AddClip &cmd) const
    {
        return json{ { "op", "addClip" },
                     { "mediaId", cmd.media_id },
                     { "trackId", cmd.track_id },
                     { "start", encode_time(cmd.start) },
                     { "ripple", cmd.ripple } };
    }
    json operator()(const AddClipAtFirstFree &cmd) const
    {
        return json{ { "op", "addClipAtFirstFree" },
                     { "mediaId", cmd.media_id },
                     { "start", encode_time(cmd.start) } };
    }
    json operator()(const AddTextClip &cmd) const
    {
        return json{ { "op", "addTextClip" },
                     { "trackId", cmd.track_id ? json(*cmd.track_id) : json(nullptr) },
                     { "above", cmd.above },
                     { "start", encode_time(cmd.start) },
                     { "style", cmd.style ? encode_text_style(*cmd.style) : json(nullptr) },
                     { "duration",
                       cmd.duration ? json(encode_time(*cmd.duration)) : json(nullptr) },
                     { "offsetY", cmd.offset_y ? json(*cmd.offset_y) : json(nullptr) } };
    }
    json operator()(const AddLayerClip &cmd) const
    {
        return json{ { "op", "addLayerClip" },
                     { "trackId", cmd.track_id ? json(*cmd.track_id) : json(nullptr) },
                     { "start", encode_time(cmd.start) },
                     { "duration",
                       cmd.duration ? json(encode_time(*cmd.duration)) : json(nullptr) },
                     { "effectId", cmd.effect_id },
                     { "name", cmd.name } };
    }
    json operator()(const SetClipSpeedCurve &cmd) const
    {
        return json{ { "op", "setClipSpeedCurve" },
                     { "clipId", cmd.clip_id },
                     { "curve", cmd.curve ? encode_speed_curve(*cmd.curve) : json(nullptr) } };
    }
    json operator()(const SetClipKey &cmd) const
    {
        return json{ { "op", "setClipKey" },
                     { "clipId", cmd.clip_id },
                     { "property", std::string(name(cmd.property)) },
                     { "at", cmd.at },
                     { "value", cmd.value },
                     { "ease", detail::write_ease(cmd.ease) } };
    }
    json operator()(const ClearClipKey &cmd) const
    {
        return json{ { "op", "clearClipKey" },
                     { "clipId", cmd.clip_id },
                     { "property", std::string(name(cmd.property)) },
                     { "at", cmd.at } };
    }
    json operator()(const ClearClipKeys &cmd) const
    {
        return json{ { "op", "clearClipKeys" },
                     { "clipId", cmd.clip_id },
                     { "property", std::string(name(cmd.property)) } };
    }
    json operator()(const SetEffectKey &cmd) const
    {
        return json{ { "op", "setEffectKey" },
                     { "clipId", cmd.clip_id },
                     { "entry", cmd.entry },
                     { "key", cmd.key },
                     { "at", cmd.at },
                     { "value", cmd.value },
                     { "ease", detail::write_ease(cmd.ease) } };
    }
    json operator()(const ClearEffectKey &cmd) const
    {
        return json{ { "op", "clearEffectKey" },
                     { "clipId", cmd.clip_id },
                     { "entry", cmd.entry },
                     { "key", cmd.key },
                     { "at", cmd.at } };
    }
    json operator()(const ClearEffectKeys &cmd) const
    {
        return json{ { "op", "clearEffectKeys" },
                     { "clipId", cmd.clip_id },
                     { "entry", cmd.entry },
                     { "key", cmd.key } };
    }
    json operator()(const SetClipCutout &cmd) const
    {
        return json{ { "op", "setClipCutout" },
                     { "clipId", cmd.clip_id },
                     { "cutout", cmd.cutout ? encode_cutout(*cmd.cutout) : json(nullptr) } };
    }
    json operator()(const AddCutoutStroke &cmd) const
    {
        return json{ { "op", "addCutoutStroke" },
                     { "clipId", cmd.clip_id },
                     { "stroke", encode_stroke(cmd.stroke) } };
    }
    json operator()(const MoveClips &cmd) const
    {
        json moves = json::array();
        for (const ClipMove &move : cmd.moves) {
            moves.push_back(encode_clip_move(move));
        }
        return json{ { "op", "moveClips" }, { "moves", std::move(moves) } };
    }
    json operator()(const TrimClip &cmd) const
    {
        return json{ { "op", "trimClip" },
                     { "clipId", cmd.clip_id },
                     { "edge", trim_edge_name(cmd.edge) },
                     { "delta", encode_time(cmd.delta) },
                     { "ripple", cmd.ripple } };
    }
    json operator()(const SplitClips &cmd) const
    {
        json clip_ids = json::array();
        for (const std::string &id : cmd.clip_ids) {
            clip_ids.push_back(id);
        }
        return json{ { "op", "splitClips" },
                     { "clipIds", std::move(clip_ids) },
                     { "time", encode_time(cmd.time) } };
    }
    json operator()(const ReplaceClipMedia &cmd) const
    {
        return json{ { "op", "replaceClipMedia" },
                     { "clipId", cmd.clip_id },
                     { "item", encode_new_media(cmd.item) },
                     { "sourceStart",
                       cmd.source_start ? json(encode_time(*cmd.source_start)) : json(nullptr) } };
    }
    json operator()(const FreezeFrame &cmd) const
    {
        return json{ { "op", "freezeFrame" },
                     { "clipId", cmd.clip_id },
                     { "time", encode_time(cmd.time) },
                     { "duration",
                       cmd.duration ? json(encode_time(*cmd.duration)) : json(nullptr) },
                     { "still", cmd.still ? encode_new_media(*cmd.still) : json(nullptr) } };
    }
    json operator()(const MergeClips &cmd) const
    {
        json clip_ids = json::array();
        for (const std::string &id : cmd.clip_ids) {
            clip_ids.push_back(id);
        }
        return json{ { "op", "mergeClips" }, { "clipIds", std::move(clip_ids) } };
    }
    json operator()(const RemoveClips &cmd) const
    {
        json clip_ids = json::array();
        for (const std::string &id : cmd.clip_ids) {
            clip_ids.push_back(id);
        }
        return json{ { "op", "removeClips" },
                     { "clipIds", std::move(clip_ids) },
                     { "ripple", cmd.ripple } };
    }
    json operator()(const UpdateClip &cmd) const
    {
        return json{ { "op", "updateClip" },
                     { "clipId", cmd.clip_id },
                     { "patch", encode_clip_patch(cmd.patch) } };
    }
    json operator()(const SetClipSpeed &cmd) const
    {
        return json{ { "op", "setClipSpeed" }, { "clipId", cmd.clip_id }, { "speed", cmd.speed } };
    }
    json operator()(const SetClipReverse &cmd) const
    {
        return json{ { "op", "setClipReverse" },
                     { "clipId", cmd.clip_id },
                     { "reverse", cmd.reverse } };
    }
    json operator()(const SetClipPan &cmd) const
    {
        return json{ { "op", "setClipPan" }, { "clipId", cmd.clip_id }, { "pan", cmd.pan } };
    }
    json operator()(const SetClipTransform &cmd) const
    {
        return json{ { "op", "setClipTransform" },
                     { "clipId", cmd.clip_id },
                     { "scale", cmd.scale ? json(*cmd.scale) : json(nullptr) },
                     { "offsetX", cmd.offset_x ? json(*cmd.offset_x) : json(nullptr) },
                     { "offsetY", cmd.offset_y ? json(*cmd.offset_y) : json(nullptr) },
                     { "rotation", cmd.rotation ? json(*cmd.rotation) : json(nullptr) },
                     { "stretchX", cmd.stretch_x ? json(*cmd.stretch_x) : json(nullptr) },
                     { "stretchY", cmd.stretch_y ? json(*cmd.stretch_y) : json(nullptr) } };
    }
    json operator()(const DetachAudio &cmd) const
    {
        return json{ { "op", "detachAudio" }, { "clipId", cmd.clip_id } };
    }
    json operator()(const ReattachAudio &cmd) const
    {
        return json{ { "op", "reattachAudio" }, { "clipId", cmd.clip_id } };
    }
    json operator()(const AddTrack &) const { return json{ { "op", "addTrack" } }; }
    json operator()(const RemoveTrack &cmd) const
    {
        return json{ { "op", "removeTrack" }, { "trackId", cmd.track_id } };
    }
    json operator()(const SetTrackFlag &cmd) const
    {
        return json{ { "op", "setTrackFlag" },
                     { "trackId", cmd.track_id },
                     { "flag", track_flag_name(cmd.flag) },
                     { "value", cmd.value } };
    }
    json operator()(const AddTimeline &) const { return json{ { "op", "addTimeline" } }; }
    json operator()(const RemoveTimeline &cmd) const
    {
        return json{ { "op", "removeTimeline" }, { "timelineId", cmd.timeline_id } };
    }
    json operator()(const SetTimelineVideo &cmd) const
    {
        return json{ { "op", "setTimelineVideo" },
                     { "timelineId", cmd.timeline_id },
                     { "video", encode_video_settings(cmd.video) } };
    }
    json operator()(const RenameTimeline &cmd) const
    {
        return json{ { "op", "renameTimeline" },
                     { "timelineId", cmd.timeline_id },
                     { "name", cmd.name } };
    }
    json operator()(const SelectTimeline &cmd) const
    {
        return json{ { "op", "selectTimeline" }, { "timelineId", cmd.timeline_id } };
    }
    json operator()(const MoveTimeline &cmd) const
    {
        return json{ { "op", "moveTimeline" },
                     { "timelineId", cmd.timeline_id },
                     { "index", cmd.index } };
    }
    json operator()(const AddFont &cmd) const
    {
        return json{ { "op", "addFont" }, { "family", cmd.family }, { "path", cmd.path } };
    }
    json operator()(const RemoveFont &cmd) const
    {
        return json{ { "op", "removeFont" }, { "family", cmd.family } };
    }
    json operator()(const UpdateMediaPath &cmd) const
    {
        return json{ { "op", "updateMediaPath" },
                     { "mediaId", cmd.media_id },
                     { "newPath", cmd.new_path } };
    }
    json operator()(const SetMediaColorRange &cmd) const
    {
        return json{ { "op", "setMediaColorRange" },
                     { "mediaId", cmd.media_id },
                     { "range",
                       cmd.range ? json(std::string(detail::color_range_name(*cmd.range)))
                                 : json(nullptr) } };
    }
};

} // namespace

nlohmann::json encode(const Command &command)
{
    return std::visit(Encoder{ }, command.value);
}

std::optional<Command> decode(const nlohmann::json &value)
{
    if (!value.is_object()) {
        return std::nullopt;
    }
    const auto op_it = value.find("op");
    if (op_it == value.end() || !op_it->is_string()) {
        return std::nullopt;
    }
    const std::string op = op_it->get<std::string>();

    if (op == "addMedia") {
        const auto it = value.find("item");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<NewMedia> item = read_new_media(*it);
        if (!item) {
            return std::nullopt;
        }
        return Command{ AddMedia{ std::move(*item) } };
    }
    if (op == "removeMedia") {
        RemoveMedia cmd;
        if (!detail::take_string(value, "mediaId", cmd.media_id)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setMediaPlaceholder") {
        SetMediaPlaceholder cmd;
        if (!detail::take_string(value, "mediaId", cmd.media_id)) {
            return std::nullopt;
        }
        if (!detail::take_bool(value, "placeholder", cmd.placeholder)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "fillSlot") {
        FillSlot cmd;
        if (!detail::take_string(value, "mediaId", cmd.media_id)) {
            return std::nullopt;
        }
        const auto it = value.find("item");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<NewMedia> item = read_new_media(*it);
        if (!item) {
            return std::nullopt;
        }
        cmd.item = std::move(*item);
        return Command{ std::move(cmd) };
    }
    if (op == "batch") {
        const auto it = value.find("commands");
        if (it == value.end() || !it->is_array()) {
            return std::nullopt;
        }
        Batch cmd;
        for (const json &entry : *it) {
            std::optional<Command> c = decode(entry);
            if (!c) {
                return std::nullopt;
            }
            cmd.commands.push_back(std::move(*c));
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addClip") {
        AddClip cmd;
        if (!detail::take_string(value, "mediaId", cmd.media_id)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "trackId", cmd.track_id)) {
            return std::nullopt;
        }
        if (!detail::take_time(value, "start", cmd.start)) {
            return std::nullopt;
        }
        if (!detail::take_bool(value, "ripple", cmd.ripple)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addClipAtFirstFree") {
        AddClipAtFirstFree cmd;
        if (!detail::take_string(value, "mediaId", cmd.media_id)) {
            return std::nullopt;
        }
        if (!detail::take_time(value, "start", cmd.start)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addTextClip") {
        AddTextClip cmd;
        if (!detail::take_opt_string(value, "trackId", cmd.track_id)) {
            return std::nullopt;
        }
        if (!detail::take_bool(value, "above", cmd.above)) {
            return std::nullopt;
        }
        if (!detail::take_time(value, "start", cmd.start)) {
            return std::nullopt;
        }
        if (const auto it = value.find("style"); it != value.end() && !it->is_null()) {
            std::optional<TextStyle> style = read_text_style(*it);
            if (!style) {
                return std::nullopt;
            }
            cmd.style = std::move(*style);
        }
        if (!detail::take_opt_time(value, "duration", cmd.duration)) {
            return std::nullopt;
        }
        if (!detail::take_opt_double(value, "offsetY", cmd.offset_y)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addLayerClip") {
        AddLayerClip cmd;
        if (!detail::take_opt_string(value, "trackId", cmd.track_id)) {
            return std::nullopt;
        }
        if (!detail::take_time(value, "start", cmd.start)) {
            return std::nullopt;
        }
        if (!detail::take_opt_time(value, "duration", cmd.duration)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "effectId", cmd.effect_id)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "name", cmd.name)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setClipSpeedCurve") {
        SetClipSpeedCurve cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("curve"); it != value.end() && !it->is_null()) {
            std::optional<std::vector<SpeedPoint>> curve = read_speed_curve(*it);
            if (!curve) {
                return std::nullopt;
            }
            cmd.curve = std::move(*curve);
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setClipKey") {
        SetClipKey cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("property"); it != value.end()) {
            if (!it->is_string()) {
                return std::nullopt;
            }
            const std::optional<KeyProperty> p = from_name(it->get<std::string>());
            if (!p) {
                return std::nullopt;
            }
            cmd.property = *p;
        }
        if (!detail::take_double(value, "at", cmd.at)) {
            return std::nullopt;
        }
        if (!detail::take_double(value, "value", cmd.value)) {
            return std::nullopt;
        }
        if (const auto it = value.find("ease"); it != value.end()) {
            const std::optional<KeyEase> ease = read_ease_strict(*it);
            if (!ease) {
                return std::nullopt;
            }
            cmd.ease = *ease;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "clearClipKey") {
        ClearClipKey cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("property"); it != value.end()) {
            if (!it->is_string()) {
                return std::nullopt;
            }
            const std::optional<KeyProperty> p = from_name(it->get<std::string>());
            if (!p) {
                return std::nullopt;
            }
            cmd.property = *p;
        }
        if (!detail::take_double(value, "at", cmd.at)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "clearClipKeys") {
        ClearClipKeys cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("property"); it != value.end()) {
            if (!it->is_string()) {
                return std::nullopt;
            }
            const std::optional<KeyProperty> p = from_name(it->get<std::string>());
            if (!p) {
                return std::nullopt;
            }
            cmd.property = *p;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setEffectKey") {
        SetEffectKey cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        const auto entry = value.find("entry");
        if (entry == value.end() || !read_size(*entry, cmd.entry)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "key", cmd.key)) {
            return std::nullopt;
        }
        if (!detail::take_double(value, "at", cmd.at)) {
            return std::nullopt;
        }
        if (!detail::take_double(value, "value", cmd.value)) {
            return std::nullopt;
        }
        if (const auto it = value.find("ease"); it != value.end()) {
            const std::optional<KeyEase> ease = read_ease_strict(*it);
            if (!ease) {
                return std::nullopt;
            }
            cmd.ease = *ease;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "clearEffectKey") {
        ClearEffectKey cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        const auto entry = value.find("entry");
        if (entry == value.end() || !read_size(*entry, cmd.entry)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "key", cmd.key)) {
            return std::nullopt;
        }
        if (!detail::take_double(value, "at", cmd.at)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "clearEffectKeys") {
        ClearEffectKeys cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        const auto entry = value.find("entry");
        if (entry == value.end() || !read_size(*entry, cmd.entry)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "key", cmd.key)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setClipCutout") {
        SetClipCutout cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("cutout"); it != value.end() && !it->is_null()) {
            std::optional<Cutout> cutout = read_cutout(*it);
            if (!cutout) {
                return std::nullopt;
            }
            cmd.cutout = std::move(*cutout);
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addCutoutStroke") {
        AddCutoutStroke cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        const auto it = value.find("stroke");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<Stroke> stroke = read_stroke(*it);
        if (!stroke) {
            return std::nullopt;
        }
        cmd.stroke = std::move(*stroke);
        return Command{ std::move(cmd) };
    }
    if (op == "moveClips") {
        MoveClips cmd;
        const auto it = value.find("moves");
        if (it == value.end() || !it->is_array()) {
            return std::nullopt;
        }
        for (const json &entry : *it) {
            std::optional<ClipMove> move = read_clip_move(entry);
            if (!move) {
                return std::nullopt;
            }
            cmd.moves.push_back(std::move(*move));
        }
        return Command{ std::move(cmd) };
    }
    if (op == "trimClip") {
        TrimClip cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("edge"); it != value.end()) {
            const std::optional<TrimEdge> edge = read_trim_edge(*it);
            if (!edge) {
                return std::nullopt;
            }
            cmd.edge = *edge;
        }
        if (!detail::take_time(value, "delta", cmd.delta)) {
            return std::nullopt;
        }
        if (!detail::take_bool(value, "ripple", cmd.ripple)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "splitClips") {
        SplitClips cmd;
        const auto it = value.find("clipIds");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<std::vector<std::string>> ids = read_string_list(*it);
        if (!ids) {
            return std::nullopt;
        }
        cmd.clip_ids = std::move(*ids);
        if (!detail::take_time(value, "time", cmd.time)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "replaceClipMedia") {
        ReplaceClipMedia cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        const auto it = value.find("item");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<NewMedia> item = read_new_media(*it);
        if (!item) {
            return std::nullopt;
        }
        cmd.item = std::move(*item);
        if (!detail::take_opt_time(value, "sourceStart", cmd.source_start)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "freezeFrame") {
        FreezeFrame cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (!detail::take_time(value, "time", cmd.time)) {
            return std::nullopt;
        }
        if (!detail::take_opt_time(value, "duration", cmd.duration)) {
            return std::nullopt;
        }
        if (const auto it = value.find("still"); it != value.end() && !it->is_null()) {
            std::optional<NewMedia> still = read_new_media(*it);
            if (!still) {
                return std::nullopt;
            }
            cmd.still = std::move(*still);
        }
        return Command{ std::move(cmd) };
    }
    if (op == "mergeClips") {
        MergeClips cmd;
        const auto it = value.find("clipIds");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<std::vector<std::string>> ids = read_string_list(*it);
        if (!ids) {
            return std::nullopt;
        }
        cmd.clip_ids = std::move(*ids);
        return Command{ std::move(cmd) };
    }
    if (op == "removeClips") {
        RemoveClips cmd;
        const auto it = value.find("clipIds");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<std::vector<std::string>> ids = read_string_list(*it);
        if (!ids) {
            return std::nullopt;
        }
        cmd.clip_ids = std::move(*ids);
        if (!detail::take_bool(value, "ripple", cmd.ripple)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "updateClip") {
        UpdateClip cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        const auto it = value.find("patch");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<ClipPatch> patch = read_clip_patch(*it);
        if (!patch) {
            return std::nullopt;
        }
        cmd.patch = std::move(*patch);
        return Command{ std::move(cmd) };
    }
    if (op == "setClipSpeed") {
        SetClipSpeed cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (!detail::take_double(value, "speed", cmd.speed)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setClipReverse") {
        SetClipReverse cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (!detail::take_bool(value, "reverse", cmd.reverse)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setClipPan") {
        SetClipPan cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (!detail::take_double(value, "pan", cmd.pan)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setClipTransform") {
        SetClipTransform cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        if (!detail::take_opt_double(value, "scale", cmd.scale)) {
            return std::nullopt;
        }
        if (!detail::take_opt_double(value, "offsetX", cmd.offset_x)) {
            return std::nullopt;
        }
        if (!detail::take_opt_double(value, "offsetY", cmd.offset_y)) {
            return std::nullopt;
        }
        if (!detail::take_opt_double(value, "rotation", cmd.rotation)) {
            return std::nullopt;
        }
        if (!detail::take_opt_double(value, "stretchX", cmd.stretch_x)) {
            return std::nullopt;
        }
        if (!detail::take_opt_double(value, "stretchY", cmd.stretch_y)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "detachAudio") {
        DetachAudio cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "reattachAudio") {
        ReattachAudio cmd;
        if (!detail::take_string(value, "clipId", cmd.clip_id)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addTrack") {
        return Command{ AddTrack{ } };
    }
    if (op == "removeTrack") {
        RemoveTrack cmd;
        if (!detail::take_string(value, "trackId", cmd.track_id)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setTrackFlag") {
        SetTrackFlag cmd;
        if (!detail::take_string(value, "trackId", cmd.track_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("flag"); it != value.end()) {
            const std::optional<TrackFlag> flag = read_track_flag(*it);
            if (!flag) {
                return std::nullopt;
            }
            cmd.flag = *flag;
        }
        if (!detail::take_bool(value, "value", cmd.value)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addTimeline") {
        return Command{ AddTimeline{ } };
    }
    if (op == "removeTimeline") {
        RemoveTimeline cmd;
        if (!detail::take_string(value, "timelineId", cmd.timeline_id)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setTimelineVideo") {
        SetTimelineVideo cmd;
        if (!detail::take_string(value, "timelineId", cmd.timeline_id)) {
            return std::nullopt;
        }
        const auto it = value.find("video");
        if (it == value.end()) {
            return std::nullopt;
        }
        std::optional<VideoSettings> video = read_video_settings(*it);
        if (!video) {
            return std::nullopt;
        }
        cmd.video = std::move(*video);
        return Command{ std::move(cmd) };
    }
    if (op == "renameTimeline") {
        RenameTimeline cmd;
        if (!detail::take_string(value, "timelineId", cmd.timeline_id)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "name", cmd.name)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "selectTimeline") {
        SelectTimeline cmd;
        if (!detail::take_string(value, "timelineId", cmd.timeline_id)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "moveTimeline") {
        MoveTimeline cmd;
        if (!detail::take_string(value, "timelineId", cmd.timeline_id)) {
            return std::nullopt;
        }
        const auto index = value.find("index");
        if (index == value.end() || !read_size(*index, cmd.index)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "addFont") {
        AddFont cmd;
        if (!detail::take_string(value, "family", cmd.family)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "path", cmd.path)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "removeFont") {
        RemoveFont cmd;
        if (!detail::take_string(value, "family", cmd.family)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "updateMediaPath") {
        UpdateMediaPath cmd;
        if (!detail::take_string(value, "mediaId", cmd.media_id)) {
            return std::nullopt;
        }
        if (!detail::take_string(value, "newPath", cmd.new_path)) {
            return std::nullopt;
        }
        return Command{ std::move(cmd) };
    }
    if (op == "setMediaColorRange") {
        SetMediaColorRange cmd;
        if (!detail::take_string(value, "mediaId", cmd.media_id)) {
            return std::nullopt;
        }
        if (const auto it = value.find("range"); it != value.end() && !it->is_null()) {
            const std::optional<ColorRange> range = detail::read_color_range(*it);
            if (!range) {
                return std::nullopt;
            }
            cmd.range = *range;
        }
        return Command{ std::move(cmd) };
    }
    return std::nullopt;
}

} // namespace genesis::project::command
