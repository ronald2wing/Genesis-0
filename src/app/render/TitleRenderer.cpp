// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors

#include "app/render/TitleRenderer.h"

#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "project/model/Text.h"
#include "render/MediaCache.h"

namespace {

using genesis::project::TextAlign;
using genesis::project::TextStyle;

// FNV-1a 64 over `bytes` - the same constants `render::MediaCache` keys its
// files with, so the title cache and the media caches share one hash rather
// than inventing a second. Pinned: a changed hash would orphan written caches.
std::uint64_t fnv1a(std::span<const std::uint8_t> bytes)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}

// The sixteen lowercase hex digits of `value`, the cache filename stem.
std::string hex16(std::uint64_t value)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kDigits[value & 0xf];
        value >>= 4;
    }
    return out;
}

// Bytes as two hex digits each, reversible - how the identity is stored in the
// CacheKey's `source` field so it round-trips without path-encoding surprises.
std::string hex_encode(std::span<const std::uint8_t> bytes)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0xf]);
    }
    return out;
}

// ---- The cache identity ----
//
// Every field the rasteriser reads to produce pixels, in a fixed order, each
// string length-prefixed so no delimiter can collide with content. A leading
// version word means a future change to the painter invalidates stale files
// rather than reusing them.
//
// Participating TextStyle fields: font_family, font_size, font_weight, italic,
// color, align, stroke_width, stroke_color, shadow, background,
// background_radius, background_padding_x, background_padding_y, line_height.
// Deliberately excluded, because the rasteriser does not read them (the host
// layout ignores them too): opacity (a clip-level transform applied at
// composite time, never baked into the PNG), tracking, max_width, max_height,
// and `content` (the renderer paints the `text` argument, not the style's
// `content`). Canvas width and height participate because they change the
// pixels.
constexpr std::uint32_t kTitleKeyVersion = 1;

void put_u8(std::vector<std::uint8_t> &out, std::uint8_t value)
{
    out.push_back(value);
}

void put_u32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    const auto raw = std::bit_cast<std::array<std::uint8_t, 4>>(value);
    out.insert(out.end(), raw.begin(), raw.end());
}

void put_u64(std::vector<std::uint8_t> &out, std::uint64_t value)
{
    const auto raw = std::bit_cast<std::array<std::uint8_t, 8>>(value);
    out.insert(out.end(), raw.begin(), raw.end());
}

void put_string(std::vector<std::uint8_t> &out, std::string_view value)
{
    put_u64(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void put_double(std::vector<std::uint8_t> &out, double value)
{
    put_u64(out, std::bit_cast<std::uint64_t>(value));
}

std::uint8_t align_byte(TextAlign align)
{
    switch (align) {
    case TextAlign::Left:
        return 0;
    case TextAlign::Center:
        return 1;
    case TextAlign::Right:
        return 2;
    }
    return 1;
}

std::string title_identity(const TextStyle &style, std::string_view text, std::uint32_t width,
                           std::uint32_t height)
{
    std::vector<std::uint8_t> bytes;
    put_u32(bytes, kTitleKeyVersion);
    put_string(bytes, style.font_family);
    put_double(bytes, style.font_size);
    put_double(bytes, style.font_weight);
    put_u8(bytes, style.italic ? 1 : 0);
    put_string(bytes, style.color);
    put_u8(bytes, align_byte(style.align));
    put_double(bytes, style.stroke_width);
    put_string(bytes, style.stroke_color);
    put_u8(bytes, style.shadow ? 1 : 0);
    put_string(bytes, style.background);
    put_double(bytes, style.background_radius);
    put_double(bytes, style.background_padding_x);
    put_double(bytes, style.background_padding_y);
    put_double(bytes, style.line_height);
    put_string(bytes, text);
    put_u32(bytes, width);
    put_u32(bytes, height);
    return std::string(bytes.begin(), bytes.end());
}

std::span<const std::uint8_t> as_bytes(const std::string &value)
{
    return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(value.data()),
                                         value.size());
}

// Strips surrounding quotes from a CSS-style family name, so `"Cabinet
// Grotesk"` resolves the same as `Cabinet Grotesk`.
std::string strip_quotes(std::string_view family)
{
    std::string_view s = family;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        s.remove_prefix(1);
        s.remove_suffix(1);
    }
    return std::string(s);
}

// A CSS colour: #rgb, #rgba, #rrggbb, #rrggbbaa (alpha last, the spelling
// `TextStyle::background` documents) or a named colour. `fallback` when the
// string names nothing parseable, so a bad colour degrades rather than fails.
QColor parse_color(std::string_view spec, QColor fallback)
{
    std::string_view s = spec;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    if (s.empty()) {
        return fallback;
    }
    if (s.front() != '#') {
        const QColor named(QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size())));
        return named.isValid() ? named : fallback;
    }
    s.remove_prefix(1);
    if (s.empty()) {
        return fallback;
    }
    for (const char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return fallback;
        }
    }
    const auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return c - 'A' + 10;
    };
    if (s.size() == 3 || s.size() == 4) {
        const int r = nib(s[0]) * 17;
        const int g = nib(s[1]) * 17;
        const int b = nib(s[2]) * 17;
        const int a = s.size() == 4 ? nib(s[3]) * 17 : 255;
        return QColor(r, g, b, a);
    }
    if (s.size() == 6 || s.size() == 8) {
        const int r = (nib(s[0]) << 4) | nib(s[1]);
        const int g = (nib(s[2]) << 4) | nib(s[3]);
        const int b = (nib(s[4]) << 4) | nib(s[5]);
        const int a = s.size() == 8 ? ((nib(s[6]) << 4) | nib(s[7])) : 255;
        return QColor(r, g, b, a);
    }
    return fallback;
}

// The font, its metrics, the measure seam built over those metrics, and the
// layout that measure produced - resolved once so painting and the reveal map
// share exactly one layout and cannot disagree.
struct Resolved
{
    QFont font;
    QFontMetricsF metrics;
    genesis::render::Measure measure;
    genesis::render::TitleLayout layout;
};

Resolved resolve(const TextStyle &style, std::string_view text, std::uint32_t canvas_width,
                 std::uint32_t canvas_height)
{
    const double canvas_h = static_cast<double>(canvas_height);
    const int pixel = std::max(1, static_cast<int>(std::lround(style.font_size * canvas_h)));

    QFont font;
    font.setFamily(QString::fromStdString(strip_quotes(style.font_family)));
    font.setPixelSize(pixel);
    font.setWeight(static_cast<QFont::Weight>(
            std::clamp(static_cast<int>(std::lround(style.font_weight)), 100, 900)));
    font.setItalic(style.italic);

    const QFontMetricsF metrics(font);
    const genesis::render::Measure measure = [metrics](std::string_view s) {
        return static_cast<float>(metrics.horizontalAdvance(
                QString::fromUtf8(s.data(), static_cast<qsizetype>(s.size()))));
    };

    genesis::render::TitleLayout layout =
            genesis::render::layout_title(style, text, canvas_width, canvas_height, measure);

    return Resolved{ font, metrics, measure, std::move(layout) };
}

// The per-word strings, in read order, recovered from the host's joined lines.
// The host joins a line's words with single spaces and never leaves a leading
// or trailing one, so splitting on single spaces returns exactly the words the
// host laid out - no second tokeniser to drift out of step with it.
std::vector<std::string> word_list(const genesis::render::TitleLayout &layout)
{
    std::vector<std::string> out;
    out.reserve(layout.words.size());
    for (const std::string &line : layout.lines) {
        std::size_t start = 0;
        while (start < line.size()) {
            const std::size_t end = line.find(' ', start);
            const std::size_t stop = end == std::string::npos ? line.size() : end;
            out.push_back(line.substr(start, stop - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    return out;
}

// The drop shadow is a fixed look: `TextStyle` declares only a bool, so the
// offset and tint are constants - black at half strength, offset a little
// right and down, scaled to the font so the shadow tracks the title's size.
constexpr int kShadowAlpha = 128;

QImage paint(const TextStyle &style, const Resolved &resolved, std::uint32_t canvas_width,
             std::uint32_t canvas_height)
{
    QImage image(static_cast<int>(canvas_width), static_cast<int>(canvas_height),
                 QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    const double canvas_h = static_cast<double>(canvas_height);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // The plate: a rounded rectangle behind the words. The host lays out only
    // the word boxes, so the block is their bounding box expanded by the
    // style's padding. No words -> no plate, so an empty title stays blank.
    if (!style.background.empty() && !resolved.layout.words.empty()) {
        const double pad_x = style.background_padding_x * canvas_h;
        const double pad_y = style.background_padding_y * canvas_h;
        const genesis::render::WordBox &first = resolved.layout.words.front();
        double min_x = first.x;
        double min_y = first.y;
        double max_x = first.x + first.width;
        double max_y = first.y + first.height;
        for (const genesis::render::WordBox &box : resolved.layout.words) {
            min_x = std::min(min_x, static_cast<double>(box.x));
            min_y = std::min(min_y, static_cast<double>(box.y));
            max_x = std::max(max_x, static_cast<double>(box.x + box.width));
            max_y = std::max(max_y, static_cast<double>(box.y + box.height));
        }
        const QColor plate = parse_color(style.background, QColor(0, 0, 0, 0));
        if (plate.alpha() != 0) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(plate);
            painter.drawRoundedRect(
                    QRectF(min_x - pad_x, min_y - pad_y, (max_x - min_x) + 2.0 * pad_x,
                           (max_y - min_y) + 2.0 * pad_y),
                    style.background_radius * canvas_h, style.background_radius * canvas_h);
        }
    }

    const QColor fill = parse_color(style.color, QColor(255, 255, 255));
    const QColor outline = parse_color(style.stroke_color, QColor(0, 0, 0));
    const double stroke_px = style.stroke_width * canvas_h;
    const double shadow_offset = std::max(1.0, style.font_size * canvas_h * 0.06);

    // One word at a time, each into the box the host laid out for it. The
    // baseline sits at the box's top plus the font's ascent, so a glyph's full
    // ascent and descent stay inside the line-height box. Shadow, then stroke,
    // then fill - the fill covers the inner half of the stroke, leaving a
    // clean outline of half the stroke width beyond each glyph's edge.
    const std::vector<std::string> words = word_list(resolved.layout);
    for (std::size_t i = 0; i < resolved.layout.words.size() && i < words.size(); ++i) {
        const genesis::render::WordBox &box = resolved.layout.words[i];
        const QPointF origin(static_cast<double>(box.x),
                             static_cast<double>(box.y) + resolved.metrics.ascent());
        QPainterPath path;
        path.addText(origin, resolved.font, QString::fromStdString(words[i]));

        if (style.shadow) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, kShadowAlpha));
            painter.drawPath(path.translated(shadow_offset, shadow_offset));
        }
        if (stroke_px > 0.0) {
            QPen pen(outline, stroke_px, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(path);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(path);
    }

    return image;
}

} // namespace

namespace genesis::app {

genesis::render::CacheKey title_cache_key(const TextStyle &style, std::string_view text,
                                          std::uint32_t canvas_width, std::uint32_t canvas_height)
{
    const TextStyle tidied = style.tidy();
    const std::string identity = title_identity(tidied, text, canvas_width, canvas_height);
    genesis::render::CacheKey key;
    key.source = std::filesystem::path(hex_encode(as_bytes(identity)));
    return key;
}

std::filesystem::path title_cache_path(const std::filesystem::path &cache_root,
                                       const TextStyle &style, std::string_view text,
                                       std::uint32_t canvas_width, std::uint32_t canvas_height)
{
    if (cache_root.empty()) {
        return { };
    }
    const std::string identity = title_identity(style.tidy(), text, canvas_width, canvas_height);
    return cache_root / "titles" / (hex16(fnv1a(as_bytes(identity))) + ".png");
}

RenderedTitle render_title(const TextStyle &style, std::string_view text,
                           std::uint32_t canvas_width, std::uint32_t canvas_height,
                           const std::filesystem::path &cache_root)
{
    const TextStyle tidied = style.tidy();
    const std::filesystem::path png =
            title_cache_path(cache_root, tidied, text, canvas_width, canvas_height);
    const Resolved resolved = resolve(tidied, text, canvas_width, canvas_height);

    // A fresh cache entry is reused: the PNG is loaded, and the reveal map and
    // word boxes are recomputed from the same layout the cached pixels were
    // painted from - cheap, and guaranteed to agree because the key matches.
    if (!png.empty()
        && genesis::render::cache_is_fresh(
                png, title_cache_key(tidied, text, canvas_width, canvas_height))) {
        QImage loaded;
        if (loaded.load(QString::fromStdString(png.string()))) {
            RenderedTitle out;
            out.image = loaded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            out.words = resolved.layout.words;
            out.reveal = genesis::render::reveal_map_for(tidied, text, canvas_width, canvas_height,
                                                         resolved.measure);
            out.cached = true;
            return out;
        }
        // A PNG that will not load reads as stale; fall through and repaint.
    }

    RenderedTitle out;
    out.image = paint(tidied, resolved, canvas_width, canvas_height);
    out.words = resolved.layout.words;
    out.reveal = genesis::render::reveal_map_for(tidied, text, canvas_width, canvas_height,
                                                 resolved.measure);
    out.cached = false;

    if (!png.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(png.parent_path(), ec);
        if (!ec && out.image.save(QString::fromStdString(png.string()), "PNG")) {
            genesis::render::cache_record(
                    png, title_cache_key(tidied, text, canvas_width, canvas_height));
        }
    }
    return out;
}

} // namespace genesis::app
