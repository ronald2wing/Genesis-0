// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// Ported from Concat's concat-project/src/doc.rs,
// SPDX-License-Identifier: GPL-3.0-or-later,
// SPDX-FileCopyrightText: 2026 Jareer and Concat contributors.
//
// Shared document primitives: the scalar accessors, enum spellings, and ease codec.

#include <array>
#include <limits>

#include "project/document/Json.h"
#include "project/Document.h"

namespace genesis::project::detail {

const char *media_kind_name(MediaKind kind)
{
    switch (kind) {
    case MediaKind::Video:
        return "video";
    case MediaKind::Audio:
        return "audio";
    case MediaKind::Image:
        return "image";
    }
    return "video";
}

const char *media_origin_name(MediaOrigin origin)
{
    switch (origin) {
    case MediaOrigin::Speech:
        return "speech";
    case MediaOrigin::Processed:
        return "processed";
    case MediaOrigin::Enhanced:
        return "enhanced";
    }
    return "speech";
}

const char *clip_kind_name(ClipKind kind)
{
    switch (kind) {
    case ClipKind::Video:
        return "video";
    case ClipKind::Audio:
        return "audio";
    case ClipKind::Image:
        return "image";
    case ClipKind::Text:
        return "text";
    case ClipKind::Layer:
        return "layer";
    }
    return "video";
}

const char *color_range_name(ColorRange range)
{
    switch (range) {
    case ColorRange::Limited:
        return "limited";
    case ColorRange::Full:
        return "full";
    }
    return "limited";
}

const char *color_space_name(ColorSpace space)
{
    switch (space) {
    case ColorSpace::Sdr:
        return "sdr";
    case ColorSpace::Hlg:
        return "hlg";
    case ColorSpace::Pq:
        return "pq";
    }
    return "sdr";
}

const char *cutout_mode_name(CutoutMode mode)
{
    switch (mode) {
    case CutoutMode::Auto:
        return "auto";
    case CutoutMode::Custom:
        return "custom";
    }
    return "auto";
}

const char *brush_tool_name(BrushTool tool)
{
    switch (tool) {
    case BrushTool::SmartBrush:
        return "smartBrush";
    case BrushTool::Brush:
        return "brush";
    case BrushTool::SmartEraser:
        return "smartEraser";
    case BrushTool::Eraser:
        return "eraser";
    }
    return "brush";
}

const char *text_align_name(TextAlign align)
{
    switch (align) {
    case TextAlign::Left:
        return "left";
    case TextAlign::Center:
        return "center";
    case TextAlign::Right:
        return "right";
    }
    return "center";
}

json write_ease(const KeyEase &ease)
{
    return json::array({ ease.values[0], ease.values[1], ease.values[2], ease.values[3] });
}

bool as_string(const json &value, std::string &out)
{
    if (!value.is_string()) {
        return false;
    }
    out = value.get<std::string>();
    return true;
}

bool as_bool(const json &value, bool &out)
{
    if (!value.is_boolean()) {
        return false;
    }
    out = value.get<bool>();
    return true;
}

bool as_double(const json &value, double &out)
{
    if (!value.is_number()) {
        return false;
    }
    out = value.get<double>();
    return true;
}

bool as_i64(const json &value, std::int64_t &out)
{
    if (!value.is_number_integer()) {
        return false;
    }
    if (value.is_number_unsigned()) {
        const std::uint64_t u = value.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return false;
        }
        out = static_cast<std::int64_t>(u);
        return true;
    }
    out = value.get<std::int64_t>();
    return true;
}

bool as_u32(const json &value, std::uint32_t &out)
{
    std::int64_t n = 0;
    if (!as_i64(value, n) || n < 0
        || n > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }
    out = static_cast<std::uint32_t>(n);
    return true;
}

bool take_string(const json &obj, const char *key, std::string &out)
{
    const auto it = obj.find(key);
    return it == obj.end() || as_string(*it, out);
}

bool take_bool(const json &obj, const char *key, bool &out)
{
    const auto it = obj.find(key);
    return it == obj.end() || as_bool(*it, out);
}

bool take_double(const json &obj, const char *key, double &out)
{
    const auto it = obj.find(key);
    return it == obj.end() || as_double(*it, out);
}

bool take_u32(const json &obj, const char *key, std::uint32_t &out)
{
    const auto it = obj.find(key);
    return it == obj.end() || as_u32(*it, out);
}

bool take_opt_double(const json &obj, const char *key, std::optional<double> &out)
{
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return true;
    }
    double value = 0.0;
    if (!as_double(*it, value)) {
        return false;
    }
    out = value;
    return true;
}

bool take_opt_u32(const json &obj, const char *key, std::optional<std::uint32_t> &out)
{
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return true;
    }
    std::uint32_t value = 0;
    if (!as_u32(*it, value)) {
        return false;
    }
    out = value;
    return true;
}

bool take_opt_string(const json &obj, const char *key, std::optional<std::string> &out)
{
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return true;
    }
    std::string value;
    if (!as_string(*it, value)) {
        return false;
    }
    out = std::move(value);
    return true;
}

bool take_opt_bool(const json &obj, const char *key, std::optional<bool> &out)
{
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return true;
    }
    bool value = false;
    if (!as_bool(*it, value)) {
        return false;
    }
    out = value;
    return true;
}

bool take_time(const json &obj, const char *key, Rational &out)
{
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return true;
    }
    const std::optional<Rational> t = decode_time(*it);
    if (!t) {
        return false;
    }
    out = *t;
    return true;
}

bool take_opt_time(const json &obj, const char *key, std::optional<Rational> &out)
{
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return true;
    }
    const std::optional<Rational> t = decode_time(*it);
    if (!t) {
        return false;
    }
    out = *t;
    return true;
}

MediaKind read_media_kind(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "audio") {
            return MediaKind::Audio;
        }
        if (s == "image") {
            return MediaKind::Image;
        }
    }
    return MediaKind::Video;
}

ClipKind read_clip_kind(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "audio") {
            return ClipKind::Audio;
        }
        if (s == "image") {
            return ClipKind::Image;
        }
        if (s == "text") {
            return ClipKind::Text;
        }
        if (s == "layer") {
            return ClipKind::Layer;
        }
    }
    return ClipKind::Video;
}

ColorSpace read_color_space(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "hlg") {
            return ColorSpace::Hlg;
        }
        if (s == "pq") {
            return ColorSpace::Pq;
        }
    }
    return ColorSpace::Sdr;
}

TextAlign read_text_align(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "left") {
            return TextAlign::Left;
        }
        if (s == "right") {
            return TextAlign::Right;
        }
    }
    return TextAlign::Center;
}

std::optional<ColorRange> read_color_range(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "limited") {
            return ColorRange::Limited;
        }
        if (s == "full") {
            return ColorRange::Full;
        }
    }
    return std::nullopt;
}

std::optional<MediaOrigin> read_media_origin(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "speech") {
            return MediaOrigin::Speech;
        }
        if (s == "processed") {
            return MediaOrigin::Processed;
        }
        if (s == "enhanced") {
            return MediaOrigin::Enhanced;
        }
    }
    return std::nullopt;
}

std::optional<CutoutMode> read_cutout_mode(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "auto") {
            return CutoutMode::Auto;
        }
        if (s == "custom") {
            return CutoutMode::Custom;
        }
    }
    return std::nullopt;
}

std::optional<Subject> read_subject(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "auto") {
            return Subject::Auto;
        }
        if (s == "person") {
            return Subject::Person;
        }
        if (s == "object") {
            return Subject::Object;
        }
    }
    return std::nullopt;
}

std::optional<BrushTool> read_brush_tool(const json &value)
{
    if (value.is_string()) {
        const std::string s = value.get<std::string>();
        if (s == "smartBrush") {
            return BrushTool::SmartBrush;
        }
        if (s == "brush") {
            return BrushTool::Brush;
        }
        if (s == "smartEraser") {
            return BrushTool::SmartEraser;
        }
        if (s == "eraser") {
            return BrushTool::Eraser;
        }
    }
    return std::nullopt;
}

KeyEase read_ease(const json &value)
{
    if (value.is_string()) {
        return KeyEase::from_name(value.get<std::string>());
    }
    if (value.is_array() && value.size() == 4) {
        std::array<double, 4> points{ };
        for (std::size_t i = 0; i < 4; ++i) {
            if (!value[i].is_number()) {
                return KeyEase::linear();
            }
            points[i] = value[i].get<double>();
        }
        return KeyEase{ points }.sane();
    }
    return KeyEase::linear();
}

} // namespace genesis::project::detail
