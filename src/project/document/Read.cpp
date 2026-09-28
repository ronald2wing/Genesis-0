// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The document's struct readers: JSON entries to model values.

#include "project/document/Internal.h"
#include "project/document/Json.h"

namespace genesis::project::detail {

std::optional<AudioTrack> read_audio_track(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    AudioTrack track{ };
    const auto index = j.find("index");
    if (index == j.end() || !as_u32(*index, track.index)) {
        return std::nullopt;
    }
    if (!take_string(j, "codec", track.codec)) {
        return std::nullopt;
    }
    if (!take_u32(j, "channels", track.channels)) {
        return std::nullopt;
    }
    if (!take_u32(j, "sampleRate", track.sample_rate)) {
        return std::nullopt;
    }
    if (!take_string(j, "title", track.title)) {
        return std::nullopt;
    }
    if (!take_string(j, "language", track.language)) {
        return std::nullopt;
    }
    return track;
}

std::optional<MediaItem> read_media_item(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    MediaItem item;
    if (!take_string(j, "id", item.id)) {
        return std::nullopt;
    }
    if (!take_string(j, "path", item.path)) {
        return std::nullopt;
    }
    if (!take_string(j, "name", item.name)) {
        return std::nullopt;
    }
    if (!take_opt_time(j, "duration", item.duration)) {
        return std::nullopt;
    }
    if (const auto it = j.find("kind"); it != j.end()) {
        item.kind = read_media_kind(*it);
    }
    if (!take_opt_u32(j, "width", item.width)) {
        return std::nullopt;
    }
    if (!take_opt_u32(j, "height", item.height)) {
        return std::nullopt;
    }
    if (!take_opt_double(j, "frameRate", item.frame_rate)) {
        return std::nullopt;
    }
    if (!take_opt_string(j, "frameRateFraction", item.frame_rate_fraction)) {
        return std::nullopt;
    }
    if (!take_opt_string(j, "videoCodec", item.video_codec)) {
        return std::nullopt;
    }
    if (!take_opt_string(j, "audioCodec", item.audio_codec)) {
        return std::nullopt;
    }
    if (!take_bool(j, "hasAudio", item.has_audio)) {
        return std::nullopt;
    }
    if (const auto it = j.find("audioTracks"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<AudioTrack> track = read_audio_track(entry)) {
                item.audio_tracks.push_back(std::move(*track));
            }
        }
    }
    if (!take_bool(j, "placeholder", item.placeholder)) {
        return std::nullopt;
    }
    if (const auto it = j.find("colorRange"); it != j.end()) {
        item.color_range = read_color_range(*it);
    }
    if (const auto it = j.find("colorSpace"); it != j.end()) {
        item.color_space = read_color_space(*it);
    }
    if (const auto it = j.find("origin"); it != j.end()) {
        item.origin = read_media_origin(*it);
    }
    return item;
}

std::optional<Track> read_track(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Track track;
    if (!take_string(j, "id", track.id)) {
        return std::nullopt;
    }
    if (!take_bool(j, "visible", track.visible)) {
        return std::nullopt;
    }
    if (!take_bool(j, "muted", track.muted)) {
        return std::nullopt;
    }
    return track;
}

std::optional<ParamKey> read_param_key(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    ParamKey key{ };
    const auto at = j.find("at");
    if (at == j.end() || !as_double(*at, key.at)) {
        return std::nullopt;
    }
    const auto value = j.find("value");
    if (value == j.end() || !as_double(*value, key.value)) {
        return std::nullopt;
    }
    if (const auto it = j.find("ease"); it != j.end()) {
        key.ease = read_ease(*it);
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
    if (id == j.end() || !as_string(*id, filter.id)) {
        return std::nullopt;
    }
    // params is a plain map of numbers: any bad value fails the whole link.
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
    if (!take_bool(j, "enabled", filter.enabled)) {
        return std::nullopt;
    }
    // keys is a tolerant map: each run keeps the keys that parse.
    if (const auto it = j.find("keys"); it != j.end() && it->is_object()) {
        for (const auto &[name, run] : it->items()) {
            if (!run.is_array()) {
                continue;
            }
            std::vector<ParamKey> keys;
            for (const json &entry : run) {
                if (std::optional<ParamKey> key = read_param_key(entry)) {
                    keys.push_back(std::move(*key));
                }
            }
            if (!keys.empty()) {
                filter.keys[name] = std::move(keys);
            }
        }
    }
    return filter;
}

std::optional<Crop> read_crop(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Crop crop{ };
    const auto left = j.find("left");
    if (left == j.end() || !as_double(*left, crop.left)) {
        return std::nullopt;
    }
    const auto top = j.find("top");
    if (top == j.end() || !as_double(*top, crop.top)) {
        return std::nullopt;
    }
    const auto right = j.find("right");
    if (right == j.end() || !as_double(*right, crop.right)) {
        return std::nullopt;
    }
    const auto bottom = j.find("bottom");
    if (bottom == j.end() || !as_double(*bottom, crop.bottom)) {
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
    if (std::optional<BrushTool> t = read_brush_tool(*tool)) {
        stroke.tool = *t;
    } else {
        return std::nullopt;
    }
    const auto size = j.find("size");
    if (size == j.end() || !as_double(*size, stroke.size)) {
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
    if (!take_opt_time(j, "at", stroke.at)) {
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
    if (std::optional<CutoutMode> m = read_cutout_mode(*mode)) {
        cutout.mode = *m;
    } else {
        return std::nullopt;
    }
    if (const auto it = j.find("subject"); it != j.end()) {
        if (std::optional<Subject> s = read_subject(*it)) {
            cutout.subject = *s;
        } else {
            return std::nullopt;
        }
    }
    if (!take_double(j, "feather", cutout.feather)) {
        return std::nullopt;
    }
    // strokes is a plain list: one bad stroke fails the whole cutout.
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

std::optional<ClipKey> read_clip_key(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    ClipKey key{ };
    const auto property = j.find("property");
    if (property == j.end() || !property->is_string()) {
        return std::nullopt;
    }
    if (std::optional<KeyProperty> p = from_name(property->get<std::string>())) {
        key.property = *p;
    } else {
        return std::nullopt;
    }
    const auto at = j.find("at");
    if (at == j.end() || !as_double(*at, key.at)) {
        return std::nullopt;
    }
    const auto value = j.find("value");
    if (value == j.end() || !as_double(*value, key.value)) {
        return std::nullopt;
    }
    if (const auto it = j.find("ease"); it != j.end()) {
        key.ease = read_ease(*it);
    }
    return key;
}

std::optional<SpeedPoint> read_speed_point(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    SpeedPoint point{ };
    const auto at = j.find("at");
    if (at == j.end() || !as_double(*at, point.at)) {
        return std::nullopt;
    }
    const auto speed = j.find("speed");
    if (speed == j.end() || !as_double(*speed, point.speed)) {
        return std::nullopt;
    }
    return point;
}

std::optional<Transition> read_transition(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Transition transition{ };
    const auto id = j.find("id");
    if (id == j.end() || !as_string(*id, transition.id)) {
        return std::nullopt;
    }
    if (!take_time(j, "duration", transition.duration)) {
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
    if (!take_string(j, "content", style.content)) {
        return std::nullopt;
    }
    if (!take_string(j, "fontFamily", style.font_family)) {
        return std::nullopt;
    }
    if (!take_double(j, "fontSize", style.font_size)) {
        return std::nullopt;
    }
    if (!take_double(j, "fontWeight", style.font_weight)) {
        return std::nullopt;
    }
    if (!take_bool(j, "italic", style.italic)) {
        return std::nullopt;
    }
    if (!take_string(j, "color", style.color)) {
        return std::nullopt;
    }
    if (const auto it = j.find("align"); it != j.end()) {
        style.align = read_text_align(*it);
    }
    if (!take_double(j, "opacity", style.opacity)) {
        return std::nullopt;
    }
    if (!take_double(j, "strokeWidth", style.stroke_width)) {
        return std::nullopt;
    }
    if (!take_string(j, "strokeColor", style.stroke_color)) {
        return std::nullopt;
    }
    if (!take_bool(j, "shadow", style.shadow)) {
        return std::nullopt;
    }
    if (!take_string(j, "background", style.background)) {
        return std::nullopt;
    }
    if (!take_double(j, "backgroundRadius", style.background_radius)) {
        return std::nullopt;
    }
    if (!take_double(j, "backgroundPaddingX", style.background_padding_x)) {
        return std::nullopt;
    }
    if (!take_double(j, "backgroundPaddingY", style.background_padding_y)) {
        return std::nullopt;
    }
    if (!take_double(j, "lineHeight", style.line_height)) {
        return std::nullopt;
    }
    if (!take_double(j, "tracking", style.tracking)) {
        return std::nullopt;
    }
    if (!take_double(j, "maxWidth", style.max_width)) {
        return std::nullopt;
    }
    if (!take_double(j, "maxHeight", style.max_height)) {
        return std::nullopt;
    }
    return style;
}

std::optional<Clip> read_clip(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Clip clip;
    if (!take_string(j, "id", clip.id)) {
        return std::nullopt;
    }
    if (!take_string(j, "trackId", clip.track_id)) {
        return std::nullopt;
    }
    if (!take_string(j, "mediaId", clip.media_id)) {
        return std::nullopt;
    }
    if (!take_string(j, "name", clip.name)) {
        return std::nullopt;
    }
    if (const auto it = j.find("kind"); it != j.end()) {
        clip.kind = read_clip_kind(*it);
    }
    if (!take_time(j, "start", clip.start)) {
        return std::nullopt;
    }
    if (!take_time(j, "duration", clip.duration)) {
        return std::nullopt;
    }
    if (!take_time(j, "sourceStart", clip.source_start)) {
        return std::nullopt;
    }
    if (!take_double(j, "volume", clip.volume)) {
        return std::nullopt;
    }
    if (!take_double(j, "pan", clip.pan)) {
        return std::nullopt;
    }
    if (!take_time(j, "fadeIn", clip.fade_in)) {
        return std::nullopt;
    }
    if (!take_time(j, "fadeOut", clip.fade_out)) {
        return std::nullopt;
    }
    if (!take_double(j, "scale", clip.scale)) {
        return std::nullopt;
    }
    if (!take_double(j, "offsetX", clip.offset_x)) {
        return std::nullopt;
    }
    if (!take_double(j, "offsetY", clip.offset_y)) {
        return std::nullopt;
    }
    if (!take_double(j, "rotation", clip.rotation)) {
        return std::nullopt;
    }
    if (!take_double(j, "stretchX", clip.stretch_x)) {
        return std::nullopt;
    }
    if (!take_double(j, "stretchY", clip.stretch_y)) {
        return std::nullopt;
    }
    if (!take_double(j, "opacity", clip.opacity)) {
        return std::nullopt;
    }
    if (!take_double(j, "speed", clip.speed)) {
        return std::nullopt;
    }
    // speedCurve: a list that parses, or none.
    if (const auto it = j.find("speedCurve"); it != j.end() && it->is_array()) {
        std::vector<SpeedPoint> points;
        for (const json &entry : *it) {
            if (std::optional<SpeedPoint> point = read_speed_point(entry)) {
                points.push_back(std::move(*point));
            }
        }
        if (!points.empty()) {
            clip.speed_curve = std::move(points);
        }
    }
    if (const auto it = j.find("keys"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<ClipKey> key = read_clip_key(entry)) {
                clip.keys.push_back(std::move(*key));
            }
        }
    }
    if (!take_bool(j, "flipH", clip.flip_h)) {
        return std::nullopt;
    }
    if (!take_bool(j, "flipV", clip.flip_v)) {
        return std::nullopt;
    }
    if (!take_bool(j, "reverse", clip.reverse)) {
        return std::nullopt;
    }
    if (!take_string(j, "blend", clip.blend)) {
        return std::nullopt;
    }
    if (const auto it = j.find("crop"); it != j.end() && !it->is_null()) {
        clip.crop = read_crop(*it);
    }
    if (const auto it = j.find("cutout"); it != j.end() && !it->is_null()) {
        clip.cutout = read_cutout(*it);
    }
    if (!take_bool(j, "preservePitch", clip.preserve_pitch)) {
        return std::nullopt;
    }
    if (const auto it = j.find("filters"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<AppliedFilter> filter = read_applied_filter(entry)) {
                clip.filters.push_back(std::move(*filter));
            }
        }
    }
    if (const auto it = j.find("videoEffects"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<AppliedFilter> filter = read_applied_filter(entry)) {
                clip.video_effects.push_back(std::move(*filter));
            }
        }
    }
    if (!take_opt_u32(j, "audioStream", clip.audio_stream)) {
        return std::nullopt;
    }
    if (!take_opt_bool(j, "muted", clip.muted)) {
        return std::nullopt;
    }
    if (!take_opt_string(j, "detachedFrom", clip.detached_from)) {
        return std::nullopt;
    }
    if (const auto it = j.find("transitionIn"); it != j.end() && !it->is_null()) {
        clip.transition_in = read_transition(*it);
    }
    if (const auto it = j.find("text"); it != j.end() && !it->is_null()) {
        clip.text = read_text_style(*it);
    }
    return clip;
}

std::optional<VideoSettings> read_video_settings(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    VideoSettings video{ };
    const auto width = j.find("width");
    if (width == j.end() || !as_u32(*width, video.width)) {
        return std::nullopt;
    }
    const auto height = j.find("height");
    if (height == j.end() || !as_u32(*height, video.height)) {
        return std::nullopt;
    }
    const auto rate_num = j.find("rateNum");
    if (rate_num == j.end() || !as_i64(*rate_num, video.rate_num)) {
        return std::nullopt;
    }
    const auto rate_den = j.find("rateDen");
    if (rate_den == j.end() || !as_i64(*rate_den, video.rate_den)) {
        return std::nullopt;
    }
    if (const auto it = j.find("colorSpace"); it != j.end()) {
        video.color_space = read_color_space(*it);
    }
    return video;
}

std::optional<Timeline> read_timeline(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    Timeline timeline;
    if (!take_string(j, "id", timeline.id)) {
        return std::nullopt;
    }
    if (!take_string(j, "name", timeline.name)) {
        return std::nullopt;
    }
    // video defaults when absent; when present its four numbers are required.
    if (const auto it = j.find("video"); it != j.end()) {
        if (std::optional<VideoSettings> video = read_video_settings(*it)) {
            timeline.video = std::move(*video);
        } else {
            return std::nullopt;
        }
    }
    if (const auto it = j.find("tracks"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<Track> track = read_track(entry)) {
                timeline.tracks.push_back(std::move(*track));
            }
        }
    }
    if (const auto it = j.find("clips"); it != j.end() && it->is_array()) {
        for (const json &entry : *it) {
            if (std::optional<Clip> clip = read_clip(entry)) {
                timeline.clips.push_back(std::move(*clip));
            }
        }
    }
    return timeline;
}

std::optional<CustomFont> read_font(const json &j)
{
    if (!j.is_object()) {
        return std::nullopt;
    }
    const auto family = j.find("family");
    if (family == j.end() || !family->is_string()) {
        return std::nullopt;
    }
    const auto path = j.find("path");
    if (path == j.end() || !path->is_string()) {
        return std::nullopt;
    }
    return CustomFont{ family->get<std::string>(), path->get<std::string>() };
}

} // namespace genesis::project::detail
